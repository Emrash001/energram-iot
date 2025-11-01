#include <WiFi.h>
//#include <Firebase_ESP_Client.h>
#include <Keypad.h>
#include <GyverOLED.h>
//#include <Adafruit_INA219.h>
#include <EEPROM.h>
#include <time.h>

// ===== Battery Configuration (3S40P - 11.1V, 88Ah) =====
#define BATTERY_MIN_VOLTAGE 9.0     // Minimum battery voltage (3.0V * 3 cells)
#define BATTERY_MAX_VOLTAGE 12.6    // Maximum battery voltage (4.2V * 3 cells)
#define BATTERY_NOMINAL_VOLTAGE 11.1 // Nominal voltage (3.7V * 3 cells)
#define VOLTAGE_SMOOTHING 0.2       // Smoothing factor (lower = more smoothing)
#define POWER_THRESHOLD 0.1         // Minimum power to be considered active (watts)
#define CHARGING_CURRENT -0.02      // Current threshold for charging (negative)
#define DISCHARGING_CURRENT 0.02    // Current threshold for discharging
#define SHUNT_VOLTAGE_CHARGING -0.01 // Shunt voltage threshold for charging
#define SHUNT_VOLTAGE_DISCHARGING 0.01 // Shunt voltage threshold for discharging

// ===== Function Prototypes =====
void showWelcomeScreen();
void showPinEntryScreen(bool showAttempts = false);
void showAccessGranted();
void showAccessDenied();
void loadSecurityState();
void saveSecurityState();
void checkLockoutStatus();
void handleLockoutScreen();
void handlePinEntry();
void resetPinEntry();
void verifyPin();
void deleteLastDigit();
void handleHomeScreen();
void updatePowerData();
void drawBatteryIcon();
void drawChargingAnimation();
float calculateBatteryPercentage(float voltage);
float mapFloat(float x, float in_min, float in_max, float out_min, float out_max);
bool isValidKeyPress(char key);
void initializeRTC();
unsigned long getRealTimeSeconds();
void saveRealTimestamp();
unsigned long loadRealTimestamp();

// ===== Hardware Configuration =====
//Adafruit_INA219 ina219;
#define relay 12
GyverOLED<SSH1106_128x64> oled;

// Keypad configuration with debouncing
const byte ROWS = 4, COLS = 3;
char keys[ROWS][COLS] = {
    {'1', '2', '3'},
    {'4', '5', '6'},
    {'7', '8', '9'},
    {'*', '0', '#'}};
byte rowPins[ROWS] = {19, 18, 5, 17};
byte colPins[COLS] = {16, 4, 0};
Keypad customKeypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ===== System Variables =====
#define CORRECT_PIN "1911"
#define MAX_ATTEMPTS 5
#define LOCKOUT_DURATION 120000 // 2 minutes in milliseconds (for testing)
char enteredPin[5] = "----";    // 4 digits + null terminator
uint8_t pinPosition = 0;
uint8_t failedAttempts = 0;
unsigned long lockoutStartTime = 0;  // When lockout started
bool authenticated = false;
bool systemLocked = false;

// EEPROM addresses
#define EEPROM_SIZE 32
#define ATTEMPTS_ADDR 0
#define LOCKOUT_START_ADDR 4
#define LOCKOUT_ACTIVE_ADDR 8
#define LOCKOUT_TIMESTAMP_ADDR 12  // Real timestamp when lockout started
#define BOOT_TIMESTAMP_ADDR 20     // Timestamp when system last booted

// Power monitoring
float loadVoltage = 0;
float current_A = 0;
float power_W = 0;
bool isCharging = false;
bool wasCharging = false;  // Previous charging state for animation
float batteryPercentage = 0;
unsigned long lastPowerUpdate = 0;
float smoothedVoltage = 0;
unsigned long lastChargeChange = 0;
#define CHARGE_DEBOUNCE 1000 // 1-second debounce for faster detection

// Charging animation variables
unsigned long lastChargingAnimUpdate = 0;
uint8_t chargingAnimFrame = 0;
#define CHARGING_ANIM_SPEED 300 // milliseconds between animation frames

// Keypad debouncing
unsigned long lastKeyPressTime = 0;
char lastKey = 0;
#define KEY_DEBOUNCE_TIME 200 // 200ms debounce time

// RTC variables
unsigned long systemBootTime = 0;
unsigned long lockoutRealStartTime = 0;

// ===== Main Functions =====
void setup()
{
    Serial.begin(115200);

    // Initialize hardware
    pinMode(relay, OUTPUT);
    digitalWrite(relay, LOW);

    // Initialize OLED
    oled.init();
    oled.clear();
    oled.update();

    // Initialize INA219
    // if (!ina219.begin())
    // {
    //     Serial.println("Failed to find INA219 chip");
    //     while (1)
    //     {
    //         delay(10);
    //     }
    // }
    // ina219.setCalibration_32V_2A(); // Set calibration for better accuracy

    // Initialize EEPROM
    EEPROM.begin(EEPROM_SIZE);
    
    // Initialize RTC and calculate boot time
    initializeRTC();
    
    // Load security state and check real-time lockout
    loadSecurityState();

    // Configure keypad debouncing
    customKeypad.setDebounceTime(50);

    // Show welcome screen
    showWelcomeScreen();
    delay(3000);

    // Check if system is locked (with real-time consideration)
    checkLockoutStatus();

    // Initial power reading
    updatePowerData();
    smoothedVoltage = loadVoltage; // Initialize smoothed voltage
}

void loop()
{
    if (systemLocked)
    {
        handleLockoutScreen();
    }
    else if (!authenticated)
    {
        handlePinEntry();
    }
    else
    {
        handleHomeScreen();
    }

    // Update power data every 500ms
    if (millis() - lastPowerUpdate > 500)
    {
        updatePowerData();
        lastPowerUpdate = millis();
    }
    
    // Small delay to prevent excessive CPU usage
    delay(50);
}

// ===== RTC and Real-Time Functions =====
void initializeRTC()
{
    // Get current system uptime as boot reference
    systemBootTime = millis();
    
    // Try to get real time from NTP (optional - requires WiFi)
    // For now, we'll use a simple elapsed time tracking system
    
    Serial.print("System boot time reference: ");
    Serial.println(systemBootTime);
}

unsigned long getRealTimeSeconds()
{
    // This returns seconds since system boot
    // In a full implementation, you'd get actual Unix timestamp
    return (millis() - systemBootTime) / 1000;
}

void saveRealTimestamp()
{
    unsigned long currentRealTime = getRealTimeSeconds();
    EEPROM.put(LOCKOUT_TIMESTAMP_ADDR, currentRealTime);
    EEPROM.put(BOOT_TIMESTAMP_ADDR, systemBootTime);
    EEPROM.commit();
    
    Serial.print("Saved real timestamp: ");
    Serial.println(currentRealTime);
}

unsigned long loadRealTimestamp()
{
    unsigned long savedTimestamp;
    unsigned long savedBootTime;
    
    EEPROM.get(LOCKOUT_TIMESTAMP_ADDR, savedTimestamp);
    EEPROM.get(BOOT_TIMESTAMP_ADDR, savedBootTime);
    
    // If data is corrupted, return 0
    if (savedTimestamp == 0xFFFFFFFF || savedBootTime == 0xFFFFFFFF)
    {
        return 0;
    }
    
    return savedTimestamp;
}

// ===== Display Functions =====
void showWelcomeScreen()
{
    oled.clear();
    oled.rect(0, 0, 127, 63, OLED_STROKE);

    // Center "Welcome to" (11 characters)
    int welcomeWidth = 11 * 6; // ~6 pixels per character
    int welcomeX = (128 - welcomeWidth) / 2;
    oled.setCursorXY(welcomeX, 20);
    oled.print("Welcome to");

    // Center "ENERGRAM" (8 characters)
    int energramWidth = 8 * 6;
    int energramX = (128 - energramWidth) / 2;
    oled.setCursorXY(energramX, 35);
    oled.print("ENERGRAM");

    oled.update();
}

void showPinEntryScreen(bool showAttempts)
{
    oled.clear();
    oled.rect(0, 0, 127, 63, OLED_STROKE);
    
    // Center "Enter your PIN:" (15 characters)
    int titleWidth = 15 * 6;
    int titleX = (128 - titleWidth) / 2;
    oled.setCursorXY(titleX, 15);
    oled.print("Enter your PIN:");

    // Center PIN display (4 characters + spacing)
    int pinWidth = 4 * 8; // Wider spacing for PIN display
    int pinX = (128 - pinWidth) / 2;
    oled.setCursorXY(pinX, 30);
    for (int i = 0; i < pinPosition; i++)
    {
        oled.print("* ");
    }
    for (int i = pinPosition; i < 4; i++)
    {
        oled.print("- ");
    }

    // Only show attempts if requested (after first failed attempt)
    if (showAttempts && failedAttempts > 0)
    {
        int attemptsWidth = 16 * 6; // Approximate width
        int attemptsX = (128 - attemptsWidth) / 2;
        oled.setCursorXY(attemptsX, 50);
        oled.print("Attempts left: ");
        oled.print(MAX_ATTEMPTS - failedAttempts);
    }

    oled.update();
}

void showAccessGranted()
{
    oled.clear();
    oled.rect(0, 0, 127, 63, OLED_STROKE);
    
    // Center "Access Granted!" (15 characters)
    int titleWidth = 15 * 6;
    int titleX = (128 - titleWidth) / 2;
    oled.setCursorXY(titleX, 30);
    oled.print("Access Granted!");
    
    oled.update();
    delay(2000);
}

void showAccessDenied()
{
    oled.clear();
    oled.rect(0, 0, 127, 63, OLED_STROKE);
    
    // Center "Incorrect PIN!" (14 characters)
    int titleWidth = 14 * 6;
    int titleX = (128 - titleWidth) / 2;
    oled.setCursorXY(titleX, 20);
    oled.print("Incorrect PIN!");
    
    // Center attempts message
    String attemptsMsg = String(MAX_ATTEMPTS - failedAttempts);
    attemptsMsg += " attempts left";
    int msgWidth = attemptsMsg.length() * 6;
    int msgX = (128 - msgWidth) / 2;
    oled.setCursorXY(msgX, 40);
    oled.print(attemptsMsg);
    
    oled.update();
    delay(2000);
}

// ===== Power Monitoring Functions =====
void updatePowerData()
{
    // Store previous charging state
    wasCharging = isCharging;
    
    // Read raw values
    float shuntVoltage =  20; //ina219.getShuntVoltage_mV() / 1000.0; // Convert to volts
    float busVoltage = 20; //ina219.getBusVoltage_V();
    current_A =  10;//ina219.getCurrent_mA() / 1000.0;
    power_W =  10; //ina219.getPower_mW() / 1000.0;
    loadVoltage = busVoltage + shuntVoltage;

    // Apply exponential smoothing to voltage readings
    smoothedVoltage = smoothedVoltage * (1.0 - VOLTAGE_SMOOTHING) + loadVoltage * VOLTAGE_SMOOTHING;

    // Calculate battery percentage
    batteryPercentage = calculateBatteryPercentage(smoothedVoltage);

    // Detect charging state with faster response
    unsigned long now = millis();
    bool newChargingState = isCharging;
    
    // More sensitive charging detection
    if (current_A <= CHARGING_CURRENT || shuntVoltage <= SHUNT_VOLTAGE_CHARGING)
    {
        newChargingState = true;
    }
    else if (current_A >= DISCHARGING_CURRENT || shuntVoltage >= SHUNT_VOLTAGE_DISCHARGING)
    {
        newChargingState = false;
    }

    // Update charging state with reduced debounce for faster detection
    if (newChargingState != isCharging)
    {
        if (now - lastChargeChange > CHARGE_DEBOUNCE)
        {
            isCharging = newChargingState;
            lastChargeChange = now;
            Serial.print("Charging state changed to: ");
            Serial.println(isCharging ? "CHARGING" : "DISCHARGING");
        }
    }
    else
    {
        lastChargeChange = now;
    }
}

float calculateBatteryPercentage(float voltage)
{
    // Constrain voltage to valid range
    voltage = 11.7;
    
    // // Calculate percentage with non-linear mapping for Li-ion characteristics
    // float percentage;
    // if (voltage < 10.5) // Below 3.5V per cell
    // {
    //     // More sensitive at lower voltages (critical range)
    //     percentage = mapFloat(voltage, BATTERY_MIN_VOLTAGE, 10.5, 0.0, 20.0);
    // }
    // else if (voltage < 11.7) // 3.5V - 3.9V per cell
    // {
    //     // Linear range
    //     percentage = mapFloat(voltage, 10.5, 11.7, 20.0, 80.0);
    // }
    // else
    // {
    //     // Less sensitive at higher voltages (full range)
    //     percentage = mapFloat(voltage, 11.7, BATTERY_MAX_VOLTAGE, 80.0, 100.0);
    // }
    
    return 57;
}

float mapFloat(float x, float in_min, float in_max, float out_min, float out_max)
{
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// ===== Home Screen Functions =====
void handleHomeScreen()
{
    oled.clear();
    oled.rect(0, 0, 127, 63, OLED_STROKE);

    // Top row: Battery icon (left) and Current (right)
    drawBatteryIcon();
    
    // Current display (top right, aligned)
    // oled.setCursorXY(85, 5);
    // oled.print(abs(current_A), 2);
    // oled.print("A");

    // Load information (center, well-aligned)
    int loadLabelX = 35;
    oled.setCursorXY(60u, 5);  // Moved down slightly for better spacing
    oled.print("Energram");
    //oled.print(abs(power_W), 2);
    //oled.print("W");

    // Voltage information (bottom, centered and aligned)
    int voltLabelX = 30;
    oled.setCursorXY(voltLabelX, 48);  // Moved down for better spacing
    oled.print("Voltage: ");
    oled.print(11.75, 2);
    oled.print("V");

    oled.update();
}

void drawBatteryIcon()
{
    // Position battery icon at top left
    int batteryX = 5;
    int batteryY = 5;

    // Draw battery outline (slightly larger for better visibility)
    oled.rect(batteryX, batteryY, batteryX + 32, batteryY + 12, OLED_STROKE);
    oled.rect(batteryX + 32, batteryY + 4, batteryX + 35, batteryY + 8, OLED_FILL);

    // Calculate fill width based on battery percentage
    int fillWidth = map(constrain(batteryPercentage, 0, 100), 0, 100, 0, 30);
    
    if (isCharging)
    {
        // Draw animated charging pattern
        drawChargingAnimation();
    }
    else if (fillWidth > 0)
    {
        // Normal battery fill
        oled.rect(batteryX + 1, batteryY + 1, batteryX + 1 + fillWidth, batteryY + 11, OLED_FILL);
    }

    // Display percentage text below icon with more space
    oled.setCursorXY(batteryX + 8, batteryY + 20);  // Increased vertical spacing
    oled.print(batteryPercentage, 0);
    oled.print("%");
}

void drawChargingAnimation()
{
    int batteryX = 5;
    int batteryY = 5;
    int fillWidth = map(constrain(batteryPercentage, 0, 100), 0, 100, 0, 30);
    
    // Update animation frame
    if (millis() - lastChargingAnimUpdate > CHARGING_ANIM_SPEED)
    {
        chargingAnimFrame = (chargingAnimFrame + 1) % 4;
        lastChargingAnimUpdate = millis();
    }
    
    // Draw base fill
    if (fillWidth > 0)
    {
        oled.rect(batteryX + 1, batteryY + 1, batteryX + 1 + fillWidth, batteryY + 11, OLED_FILL);
    }
    
    // Draw animated charging effect
    switch (chargingAnimFrame)
    {
        case 0:
            // Lightning bolt in center
            oled.line(batteryX + 15, batteryY + 3, batteryX + 18, batteryY + 6, OLED_STROKE);
            oled.line(batteryX + 18, batteryY + 6, batteryX + 14, batteryY + 6, OLED_STROKE);
            oled.line(batteryX + 14, batteryY + 6, batteryX + 17, batteryY + 9, OLED_STROKE);
            break;
        case 1:
            // Moving segments
            for (int i = 0; i < 30; i += 6)
            {
                if (i < fillWidth)
                {
                    oled.line(batteryX + 1 + i, batteryY + 1, batteryX + 1 + i + 2, batteryY + 1, OLED_CLEAR);
                }
            }
            break;
        case 2:
            // Lightning bolt slightly offset
            oled.line(batteryX + 16, batteryY + 3, batteryX + 19, batteryY + 6, OLED_STROKE);
            oled.line(batteryX + 19, batteryY + 6, batteryX + 15, batteryY + 6, OLED_STROKE);
            oled.line(batteryX + 15, batteryY + 6, batteryX + 18, batteryY + 9, OLED_STROKE);
            break;
        case 3:
            // Full fill with pulse effect
            if (fillWidth < 30)
            {
                // Add extra segments to show charging progress
                int extraWidth = min(5, 30 - fillWidth);
                oled.rect(batteryX + 1 + fillWidth, batteryY + 1, 
                         batteryX + 1 + fillWidth + extraWidth, batteryY + 11, OLED_FILL);
            }
            break;
    }
}

// ===== Security Functions =====
void checkLockoutStatus()
{
    if (failedAttempts >= MAX_ATTEMPTS && lockoutStartTime > 0)
    {
        unsigned long currentTime = millis();
        unsigned long elapsedTime;
        
        // Load the real timestamp when lockout started
        unsigned long lockoutRealTime = loadRealTimestamp();
        unsigned long currentRealTime = getRealTimeSeconds();
        
        // If we have a valid real timestamp, use it for more accurate tracking
        if (lockoutRealTime > 0)
        {
            unsigned long realElapsedSeconds = currentRealTime - lockoutRealTime;
            elapsedTime = realElapsedSeconds * 1000; // Convert to milliseconds
            
            Serial.print("Real-time lockout check - Elapsed: ");
            Serial.print(realElapsedSeconds);
            Serial.println(" seconds");
        }
        else
        {
            // Fallback to millis() based timing
            if (currentTime >= lockoutStartTime)
            {
                elapsedTime = currentTime - lockoutStartTime;
            }
            else
            {
                // Overflow occurred
                elapsedTime = (0xFFFFFFFF - lockoutStartTime) + currentTime + 1;
            }
        }
        
        if (elapsedTime < LOCKOUT_DURATION)
        {
            systemLocked = true;
            Serial.println("System locked - lockout period active");
        }
        else
        {
            // Lockout period expired - reset everything
            Serial.println("Lockout period expired - resetting");
            failedAttempts = 0;
            lockoutStartTime = 0;
            lockoutRealStartTime = 0;
            systemLocked = false;
            saveSecurityState();
        }
    }
    else if (failedAttempts < MAX_ATTEMPTS)
    {
        // Not locked
        systemLocked = false;
        lockoutStartTime = 0;
        lockoutRealStartTime = 0;
    }
}

void handleLockoutScreen()
{
    unsigned long currentTime = millis();
    unsigned long elapsedTime;
    
    // Try to use real-time tracking first
    unsigned long lockoutRealTime = loadRealTimestamp();
    unsigned long currentRealTime = getRealTimeSeconds();
    
    if (lockoutRealTime > 0)
    {
        unsigned long realElapsedSeconds = currentRealTime - lockoutRealTime;
        elapsedTime = realElapsedSeconds * 1000;
    }
    else
    {
        // Fallback to millis() timing
        if (currentTime >= lockoutStartTime)
        {
            elapsedTime = currentTime - lockoutStartTime;
        }
        else
        {
            elapsedTime = (0xFFFFFFFF - lockoutStartTime) + currentTime + 1;
        }
    }
    
    // Check if lockout period has expired
    if (elapsedTime >= LOCKOUT_DURATION)
    {
        systemLocked = false;
        failedAttempts = 0;
        lockoutStartTime = 0;
        lockoutRealStartTime = 0;
        saveSecurityState();
        resetPinEntry();
        showPinEntryScreen(false);
        return;
    }
    
    unsigned long remainingTime = LOCKOUT_DURATION - elapsedTime;
    unsigned long minutes = remainingTime / 60000;
    unsigned long seconds = (remainingTime % 60000) / 1000;

    oled.clear();
    oled.rect(0, 0, 127, 63, OLED_STROKE);
    
    // Center "System Locked" (13 characters)
    int titleWidth = 13 * 6;
    int titleX = (128 - titleWidth) / 2;
    oled.setCursorXY(titleX, 10);
    oled.print("System Locked");
    
    // Center "Try again in" (12 characters)
    int subtitleWidth = 12 * 6;
    int subtitleX = (128 - subtitleWidth) / 2;
    oled.setCursorXY(subtitleX, 25);
    oled.print("Try again in");
    
    // Center countdown timer
    String timeStr = String(minutes);
    timeStr += ":";
    if (seconds < 10) timeStr += "0";
    timeStr += String(seconds);
    int timeWidth = timeStr.length() * 6;
    int timeX = (128 - timeWidth) / 2;
    oled.setCursorXY(timeX, 40);
    oled.print(timeStr);
    
    oled.update();
}

// ===== PIN Entry Functions =====
bool isValidKeyPress(char key)
{
    unsigned long currentTime = millis();
    
    // Check if enough time has passed since last key press
    if (currentTime - lastKeyPressTime < KEY_DEBOUNCE_TIME)
    {
        return false;
    }
    
    // Check if it's the same key pressed repeatedly too quickly
    if (key == lastKey && currentTime - lastKeyPressTime < KEY_DEBOUNCE_TIME * 2)
    {
        return false;
    }
    
    lastKeyPressTime = currentTime;
    lastKey = key;
    return true;
}

void handlePinEntry()
{
    static bool firstRun = true;
    if (firstRun)
    {
        showPinEntryScreen(false); // Don't show attempts on first run
        firstRun = false;
    }

    char key = customKeypad.getKey();
    if (key && isValidKeyPress(key))
    {
        Serial.print("Key pressed: ");
        Serial.println(key);
        
        if (key == '#')
        {
            // Delete last digit (backspace)
            deleteLastDigit();
            showPinEntryScreen(failedAttempts > 0);
        }
        else if (isdigit(key) && pinPosition < 4)
        {
            // Add digit to PIN
            enteredPin[pinPosition] = key;
            pinPosition++;
            showPinEntryScreen(failedAttempts > 0);

            // Auto-submit when 4 digits entered
            if (pinPosition == 4)
            {
                delay(500); // Brief pause to show complete PIN
                verifyPin();
            }
        }
    }
}

void deleteLastDigit()
{
    if (pinPosition > 0)
    {
        pinPosition--;
        enteredPin[pinPosition] = '-';
    }
}

void resetPinEntry()
{
    pinPosition = 0;
    memset(enteredPin, '-', 4);
    enteredPin[4] = '\0';
}

void verifyPin()
{
    // Null-terminate the entered PIN for comparison
    enteredPin[4] = '\0';
    
    Serial.print("Verifying PIN: ");
    for (int i = 0; i < 4; i++)
    {
        Serial.print(enteredPin[i]);
    }
    Serial.println();
    
    if (strncmp(enteredPin, CORRECT_PIN, 4) == 0)
    {
        // Correct PIN
        Serial.println("PIN correct - access granted");
        authenticated = true;
        failedAttempts = 0;
        lockoutStartTime = 0;
        lockoutRealStartTime = 0;
        saveSecurityState();
        showAccessGranted();
        digitalWrite(relay, HIGH);
    }
    else
    {
        // Incorrect PIN
        Serial.println("PIN incorrect - access denied");
        failedAttempts++;
        
        if (failedAttempts >= MAX_ATTEMPTS)
        {
            // Initiate lockout with real-time tracking
            lockoutStartTime = millis();
            lockoutRealStartTime = getRealTimeSeconds();
            systemLocked = true;
            
            // Save the real timestamp
            saveRealTimestamp();
            
            Serial.print("Lockout initiated at time: ");
            Serial.print(lockoutStartTime);
            Serial.print(" (real time: ");
            Serial.print(lockoutRealStartTime);
            Serial.println(")");
        }
        
        saveSecurityState();
        showAccessDenied();
        resetPinEntry();
        
        if (failedAttempts < MAX_ATTEMPTS)
        {
            showPinEntryScreen(true); // Show attempts remaining
        }
    }
}

// ===== EEPROM Functions =====
void loadSecurityState()
{
    // Read failed attempts from EEPROM
    EEPROM.get(ATTEMPTS_ADDR, failedAttempts);
    
    // Read lockout start time from EEPROM
    EEPROM.get(LOCKOUT_START_ADDR, lockoutStartTime);
    
    // Read lockout active flag
    uint8_t lockoutActive;
    EEPROM.get(LOCKOUT_ACTIVE_ADDR, lockoutActive);

    // Validate loaded data
    if (failedAttempts > MAX_ATTEMPTS || failedAttempts == 255)
    {
        failedAttempts = 0;
    }
    
    // Check if lockout time is reasonable (not corrupted)
    unsigned long currentTime = millis();
    if (lockoutStartTime > currentTime + LOCKOUT_DURATION || lockoutStartTime == 0xFFFFFFFF)
    {
        lockoutStartTime = 0;
        failedAttempts = 0;
    }
    
    Serial.print("Loaded state - Attempts: ");
    Serial.print(failedAttempts);
    Serial.print(", Lockout start: ");
    Serial.println(lockoutStartTime);
}

void saveSecurityState()
{
    // Write failed attempts to EEPROM
    EEPROM.put(ATTEMPTS_ADDR, failedAttempts);
    
    // Write lockout start time to EEPROM
    EEPROM.put(LOCKOUT_START_ADDR, lockoutStartTime);
    
    // Write lockout active flag
    uint8_t lockoutActive = systemLocked ? 1 : 0;
    EEPROM.put(LOCKOUT_ACTIVE_ADDR, lockoutActive);

    EEPROM.commit();
    
    Serial.print("Saved state - Attempts: ");
    Serial.print(failedAttempts);
    Serial.print(", Lockout start: ");
    Serial.println(lockoutStartTime);
}