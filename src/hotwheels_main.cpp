#include <iostream>
#include <chrono>
#include <thread>
#include <cmath>
#include <csignal>
#include "SampleAppsHelper.h"
#include "rsi.h"

using namespace RSI::RapidCode;
using namespace std;

// === CONSTANTS ===
constexpr double SENSOR_DISTANCE = 0.1; // meters
constexpr double ANGLE_OFFSET = 0;      // degrees
constexpr double GRAVITY = 9.81;
constexpr double UNITS_PER_DEGREE = 186413.5111;
constexpr double UNITS_PER_METER = 8532248;
constexpr double MIN_CATCHER_POSITION = 0;
constexpr double MAX_CATCHER_POSITION = 0.84;
constexpr double RAMP_HEIGHT = 0.23; //relative to catcher
constexpr bool DEBUG_MODE = true;

// === ENUMS ===
enum AxisID
{
    RAMP = 0,
    DOOR = 1,
    CATCHER = 2
};

// === GLOBALS ===
MotionController *controller = nullptr;
Axis *motorRamp = nullptr;
Axis *motorDoor = nullptr;
Axis *motorCatcher = nullptr;
IOPoint *sensor1Input = nullptr;
IOPoint *sensor2Input = nullptr;

volatile sig_atomic_t gShutdown = 0;

// === SIGNAL HANDLING ===
void SignalHandler(int signal)
{
    cout << "[Signal] Shutdown requested." << endl;
    gShutdown = 1;
    if (motorRamp)
        motorRamp->AmpEnableSet(false);
    if (motorDoor)
        motorDoor->AmpEnableSet(false);
    if (motorCatcher)
        motorCatcher->AmpEnableSet(false);
}

// === RMP SETUP ===
void InitMotor(Axis *axis)
{
    if (axis == motorCatcher){
        axis->UserUnitsSet(UNITS_PER_METER);
    }
    else{
        axis->UserUnitsSet(UNITS_PER_DEGREE);
    }
    axis->ErrorLimitTriggerValueSet(0.5);
    axis->ErrorLimitActionSet(RSIAction::RSIActionNONE);

    axis->HardwareNegLimitTriggerStateSet(1);
    axis->HardwarePosLimitTriggerStateSet(1);
    axis->HardwareNegLimitActionSet(RSIAction::RSIActionNONE);
    axis->HardwarePosLimitActionSet(RSIAction::RSIActionNONE);
    axis->HardwareNegLimitDurationSet(2);
    axis->HardwarePosLimitDurationSet(2);
    axis->PositionSet(0);
    if(axis == motorCatcher){
        axis->HomeActionSet(RSIAction::RSIActionDONE);
    }

    axis->ClearFaults();
    axis->AmpEnableSet(true);
}

void MoveSCurve(Axis *axis, double pos)
{
    try
    {
        //  Motion parameters — tune as needed
        double velocity = 50.0;      // deg/sec
        double acceleration = 300.0; // deg/sec²
        double deceleration = 300.0; // deg/sec²
        double jerkPercent = 0.0;    // 0 = trapezoidal
        if (axis == motorDoor)
        {
            //  Motion parameters for Door— tune as needed
            velocity = 100000.0;     // deg/sec
            acceleration = 300000.0; // deg/sec²
            deceleration = 300000.0; // deg/sec²
            jerkPercent = 0.0;       // 0 = trapezoidal
        }
        if (axis == motorCatcher){
            cout << "[Catcher] Moving Catcher\n";
            //  Motion parameters for Catcher—tune as needed
            velocity = 20.0;     // m/sec
            acceleration = 75.0; // m/sec²
            deceleration = 75.0; // m/sec²
            jerkPercent = 0.0;       // 0 = trapezoidal
        }
        axis->MoveSCurve(pos, velocity, acceleration, deceleration, jerkPercent);
    }
    catch (const std::exception &e)
    {
        cerr << "[Error] Move failed: " << e.what() << endl;
    }
}

void SetupRMP()
{
    MotionController::CreationParameters p;
    strncpy(p.RmpPath, "/rsi/", p.PathLengthMaximum);
    strncpy(p.NicPrimary, "enp6s0", p.PathLengthMaximum);
    p.CpuAffinity = 3;

    controller = MotionController::Create(&p);
    SampleAppsHelper::CheckErrors(controller);

    SampleAppsHelper::StartTheNetwork(controller);

    // Motor setup
    motorRamp = controller->AxisGet(RAMP);
    motorDoor = controller->AxisGet(DOOR);
    motorCatcher = controller->AxisGet(CATCHER);
    InitMotor(motorCatcher);
    InitMotor(motorRamp);
    InitMotor(motorDoor);
    cout << "[RMP] Motors initialized.\n";

    try
    {
        int sensorNodeIndex = 1; // AKD = second node on the network

        // ✅ Create IOPoint from network node (not axis)
        sensor1Input = IOPoint::CreateDigitalInput(controller->NetworkNodeGet(sensorNodeIndex), 1); // Input 1
        sensor2Input = IOPoint::CreateDigitalInput(controller->NetworkNodeGet(sensorNodeIndex), 0); // Input 0

        cout << "[I/O] Digital inputs created successfully.\n";
    }
    catch (const std::exception &e)
    {
        cerr << "[ERROR] Failed to create digital inputs: " << e.what() << endl;
        exit(1);
    }
}

double ReadSensor(IOPoint *sensorInput)
{

    if (!sensorInput)
    {
        cerr << "[ERROR] Sensor pointer is null.\n";
        return 0.0;
    }

    auto start = chrono::steady_clock::now();

    while (true)
    {
        try
        {
            bool val = sensorInput->Get();
            if (DEBUG_MODE)
            {
                cout << "[Debug] Sensor value: " << val << endl;
            }
            if (val == 0.0)
            {
                return 0.0;
            }
            return chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count();
        }
        catch (const std::exception &ex)
        {
            cerr << "[ERROR] Sensor read failed: " << ex.what() << " | Pointer: " << sensorInput << endl;
            return 0.0;
        }

        if (chrono::steady_clock::now() - start > chrono::seconds(5))
        {
            cerr << "[Warning] Sensor timeout.\n";
            return 0.0;
        }

        this_thread::sleep_for(chrono::milliseconds(1));
    }
}

// === PHYSICS ===
double ComputeSpeed(double t1, double t2)
{
    return (t2 > t1) ? SENSOR_DISTANCE / (t2 - t1) : 0.0;
}

double ComputeLandingPosition(double speed, double angleDeg)
{
    double angleRad = angleDeg * M_PI / 180.0;
    double vx = speed * cos(angleRad);
    double vy = speed * sin(angleRad);
    double timeUp = vy/GRAVITY;
    double maxHeight = vy*timeUp + 0.5*GRAVITY*timeUp*timeUp;
    double timeDown = sqrt((2*(maxHeight+RAMP_HEIGHT))/GRAVITY);
    double timeOfFlight = timeUp+timeDown;
    return vx * timeOfFlight;
}

int main()
{
    std::signal(SIGINT, SignalHandler);
    cout << "[HotWheels] Starting demo...\n";
    // motorRamp->AmpEnableSet(false);

    try
    {
        SetupRMP();
        if (!controller)
        {
            cerr << "[Fatal] Controller pointer is null after SetupRMP." << endl;
            return 1;
        }

        while (!gShutdown)
        {
            cout << "\n=== New Launch ===" << endl;

            double rampAngle;
            cout << "Enter ramp angle (degrees): ";
            cin >> rampAngle;
            if (rampAngle == 1.23){
                gShutdown = true;
            }
            // account for angle offset
            rampAngle = rampAngle - ANGLE_OFFSET;

            // 1. Set ramp angle
            MoveSCurve(motorRamp, rampAngle);
            MoveSCurve(motorDoor, 0);
            //MoveSCurve(motorCatcher, 0);

            // 2. Wait for sensor 1 — car approaching gate
            double t1 = 0.0, t2 = 0.0;
            cout << "[Sensor] Waiting for sensor 1..." << endl;
            while (t1 == 0.0)
            {
                t1 = ReadSensor(sensor1Input);
                if (DEBUG_MODE)
                {
                    cout << "[Debug] t1 value: " << t1 << endl;
                }
                this_thread::sleep_for(chrono::milliseconds(1));
            }

            // 3. Open door to let car through
            cout << "[Gate] Opening door!" << endl;
            MoveSCurve(motorDoor, 100 - rampAngle);

            // 4. Wait for sensor 2 — car passed
            cout << "[Sensor] Waiting for sensor 2..." << endl;
            while (t2 == 0.0)
            {
                t2 = ReadSensor(sensor2Input);
                if (DEBUG_MODE)
                {
                    cout << "[Debug] t2 value: " << t2 << endl;
                }
                this_thread::sleep_for(chrono::milliseconds(1));
            }

            // 5. Close door again
            cout << "[Gate] Closing door." << endl;
            MoveSCurve(motorDoor, 0.0);

            // 6. Compute physics
            double speed = ComputeSpeed(t1, t2);
            double landing = ComputeLandingPosition(speed, rampAngle);
            landing = std::clamp(landing, MIN_CATCHER_POSITION, MAX_CATCHER_POSITION);

            cout << "[Physics] Speed: " << speed << " m/s | Landing: " << landing << " m" << endl;

            // 7. Move catcher
            MoveSCurve(motorCatcher, landing);

            this_thread::sleep_for(chrono::seconds(3));
        }
    }
    catch (const std::exception &ex)
    {
        cerr << "[Fatal] Exception: " << ex.what() << endl;
    }

    // --- Shutdown Cleanup ---
    cout << "[Shutdown] Cleaning up...\n";
    if (controller)
    {
        try
        {
            motorRamp->AmpEnableSet(false);
            motorDoor->AmpEnableSet(false);
            motorCatcher->AmpEnableSet(false);
            controller->Delete();
        }
        catch (...)
        {
            cerr << "[Cleanup] Error disabling motors or deleting controller.\n";
        }
    }

    cout << "[HotWheels] Demo finished.\n";
    return 0;
}