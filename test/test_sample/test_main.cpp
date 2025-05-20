// <llm-snippet-file>test/test_sample/test_main.cpp</llm-snippet-file>
#include <Arduino.h>  // Optional if you are testing Arduino-based code
#include <unity.h>    // Unity framework
#include "main.h"  // Include the header file where playBeep is declared
#include "main.cpp"  // Include the header file where playBeep is declared

// Function to test (can replace this mock with the real one from your code)
int add(int a, int b) {
    return a + b;
}

// Test case 1: Basic addition
void test_addition_should_return_correct_result(void) {
    TEST_ASSERT_EQUAL(5, add(2, 3)); // 2 + 3 = 5
    TEST_ASSERT_EQUAL(0, add(-1, 1)); // -1 + 1 = 0
    TEST_ASSERT_EQUAL(-3, add(-1, -2)); // -1 + -2 = -3
}

// Test case 2: Edge cases
void test_addition_with_only_zeros(void) {
    TEST_ASSERT_EQUAL(0, add(0, 0)); // 0 + 0 = 0
}

// Test case 3
void test_play_beep(void) {
    playBeep();
    TEST_ASSERT_EQUAL(0, add(0, 0)); // 0 + 0 = 0
}

void setup() {

    // Initialize M5Stack Tough
    auto cfg = M5.config();
    cfg.external_spk = true; // Enable the external speaker if available
    M5.begin(cfg);
    M5.Speaker.begin();
    M5.Speaker.setVolume(200);
    M5.Display.setRotation(1);
    M5.Display.setTextSize(2);

    // Setup runs once; required for Arduino compatibility or pre-test preps
    UNITY_BEGIN();               // Start Unity test framework
    RUN_TEST(test_addition_should_return_correct_result);
    RUN_TEST(test_addition_with_only_zeros);
    RUN_TEST(test_play_beep);
    UNITY_END();                 // End Unity test framework
}

void loop() {
    // Empty loop (not used in tests)
}