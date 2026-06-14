
//Work Cited: (for beyond course concepts)
//https://www.hackster.io/mertarduino/make-a-project-that-speaks-reacts-with-the-arduino-38197b
//https://docs.arduino.cc/libraries/sd/
//https://github.com/TMRh20/TMRpcm
//https://projecthub.arduino.cc/tropicalbean/how-to-use-a-photoresistor-1143fd

/* 
1. Uses photoresistor to monitor brightness
2. Potentiometer maps the volume from 1-7
3. Processing: If light is below threshold, it trigger night mode, else it reamins in day mode
4. In night mode, SD card reader gets gthe .wav files and plays them via pin 9. The amp module boots the signal to the speaker
5. Hardware interrupts let user skip tracks or pause/play
6. LCD provides real-time data on light levels, and the current active state and/or tracks
*/

#include <SD.h>
#include <TMRpcm.h>
#include <Wire.h>
#include <TimeOut.h>
#include <DFRobot_RGBLCD1602.h>

//defining the sd card reader chip for SPI communication
#define SD_ChipSelectPin 4

const int speakerPin = 9; // PWM output for audio signal
const int lightSensorPin = A0; // input for analog photoresistor
const int POT_PIN =A1; // input for volume done by potentiometer
const int btnNext = 2; // interrupt button for next (skip) track 
const int btnPause = 3; // interrupt button for play/pause

//FSM states and actions
enum StateType {INIT_S, DAY_S, NIGHT_S, PAUSE_S};
enum Action {INIT_A, MONITOR_A, PLAY_A, STOP_A};

// used later for the LCD display
const char* stateNames[] = {"INIT", "DAY", "NIGHT", "PAUSE"};
const char* trackNames[] = {"Rain", "White", "Wild"}; 

//function prototypes
void InitSystem(); 
void MonitorLight();
void PlayAudio(); 
void StopAudio();
void systemCallback(); 
void nextISR(); 
void pauseISR();
void updateVolumeDisplay(int volume); // helper function

//function pointer taple to help map the FSM actions
static void (*action_table[])(void) = {InitSystem, MonitorLight, PlayAudio, StopAudio}; 


StateType curr_state = INIT_S; // current state at start
Action curr_action = INIT_A;  // current action at start
volatile bool nextFlag = false; // flags for interrupts
volatile bool pauseFlag = false; //flags for interrupts

volatile unsigned long lastInterruptTime =0; // to debounce the buttons
int currentTrack = 1; // the first track it starts on
int lightThreshold = 400; // trigger for it being considered "DARK"
// cause we know that 0 = Pitch black, and 1023 = very bright. 
// picked 400 as a good level less that half, meaning its gonna be dark at that reading

int lastVolume = -1; // to track volume changes 

//creating classes
TMRpcm audio; 
DFRobot_RGBLCD1602 lcd (0x6B, 16, 2); 
TimeOut systemTimeout; 


void setup(){
  // intializing inputs and outputs
  pinMode(btnNext, INPUT_PULLUP);
  pinMode(btnPause, INPUT_PULLUP); 
  pinMode(speakerPin, OUTPUT);

  //falling edge for hardware interrupts
  attachInterrupt(digitalPinToInterrupt(btnNext), nextISR, FALLING); 
  attachInterrupt(digitalPinToInterrupt(btnPause), pauseISR, FALLING);

  audio.speakerPin = speakerPin; // telling what pin is for speaker

  // using timeout every 100ms to sense/react while the audio plays
  systemTimeout.timeOut(100, systemCallback); 
}

void loop(){
  //timeout handlers to check for any trigger
  systemTimeout.handler(); 
}


void systemCallback(){
  // executes the current action in the cycle
  action_table[curr_action]();
  systemTimeout.timeOut(100, systemCallback); 
}

void InitSystem(){
  lcd.init(); 
  audio.quality(1); // 2x sampling for 8 bit PCM
  lcd.setRGB(255,255,255);

  // for SPI BUS: SD card has to be inserted before audio data can start being fetched and manipulated
  if (!SD.begin(SD_ChipSelectPin)){
    lcd.print("SD not working"); 
    while (1); // to trap it in the loop to prevent further execution of remaining code
  }

  lcd.setCursor(0,0);
  lcd.print ("SYSTEM: INIT");

  audio.setVolume(5); 
  lastVolume = 5; 

  curr_state = DAY_S; 
  curr_action = MONITOR_A; 
  lcd.clear();
}

void MonitorLight(){
  int light = analogRead(lightSensorPin);  // reading the analog voltafe from photoresistor 
  int potValue = analogRead(POT_PIN);

  //user can adjust the potentiometer to change the volume of the audio output form 1 to 7
  int volume = map(potValue, 0, 1023, 1, 7); 
  
  //volume only updates if the dial of the potentiometer changes position
  if (volume!=lastVolume){
    audio.setVolume(volume);
    lastVolume = volume; 
    updateVolumeDisplay(volume); 
  }

  //lcd output
  lcd.setCursor(0,0); 
  lcd.print("L:");
  lcd.print(light); 
  lcd.print(" "); 
  
  lcd.setCursor(6,0); 
  lcd.print("["); 
  lcd.print(stateNames[curr_state]); 
  lcd.print("]    "); 

  updateVolumeDisplay(lastVolume); 

  //state transitons:
  //for night
  // if light less then the threshold it is currently night so the audio starts playing
  if (light < lightThreshold && curr_state == DAY_S){
    curr_state = NIGHT_S; 
    curr_action = PLAY_A; // transition action
    return; 
  }

  // day
  //if greater than the threshold, it is currently day, so audio stops. +50 to prevent audio glitch
  if (light >=(lightThreshold + 50) && curr_state!=DAY_S){
    curr_state = DAY_S; 
    curr_action = STOP_A; //fsm transiton action
    return; 
  }

  // if pause/play
  if (pauseFlag){
    pauseFlag = false; 
    if (curr_state == NIGHT_S){ // if night 
      curr_state = PAUSE_S;
      curr_action = STOP_A; //stop the audio
    }
    else if (curr_state == PAUSE_S){// if already in a pause, play it 
      curr_state = NIGHT_S; 
      curr_action = PLAY_A; // re-triggering play transition
    }
  }


  if (nextFlag){ // to play the next track
    nextFlag = false;

    currentTrack ++;  // increase counter to go to next track

    if (currentTrack >3){ // I only have 3 tracks on the SD card, so it loops back around to 1 again
      currentTrack = 1;
    }

    if (curr_state == PAUSE_S){ // if in a pause already
      curr_state = NIGHT_S; // go back into the night state
      curr_action = PLAY_A; // and play the next track
      return; 
    }
    else if (curr_state == NIGHT_S){ // if in a night state force immediate play of new track
      curr_action = PLAY_A; 
      return; 
    }
  }
  
}

void PlayAudio(){ 
  char fileName[12]; // empty to store result
  // formatting the interger track number (cause mine are names 1.wav, 2.wav, etc) into a string for the SD library
  sprintf(fileName, "%d.wav", currentTrack);
  // the files are names 1.wav, 2.wav, and 3.wav so %d.wav reads the integer
  // so if currentTrack =1, it becomes, 1.wav 
  audio.play(fileName); 
  // update LCD to display current playback
  lcd.setCursor (0,1); 
  lcd.print ("ON:");
  lcd.print (trackNames[currentTrack-1]);
  lcd.print ("   "); 
  updateVolumeDisplay(lastVolume); 
  curr_action = MONITOR_A;  // fsm: play is a transition action, returns to monitor 
}


void StopAudio(){
  if (curr_state == DAY_S || curr_state == PAUSE_S){ // if in day state
    audio.stopPlayback();  //stop the audio
    digitalWrite(speakerPin, LOW); // manually forcing pwm to 0V to stop hums of glitchy noise
  }
  // clearing the LCD
  lcd.setCursor(0,1); 
  lcd.print("          "); 
  lcd.setCursor(0,1); 

  // if day is int state print thats its in it
  if (curr_state == DAY_S){
    lcd.print("OFF     "); 
  }
  else{
    // else just state the audios currently pa
    lcd.print("PAUSED "); 
  }
  updateVolumeDisplay(lastVolume); 
  curr_action = MONITOR_A; // returning to monitoring for any light changes
}

void nextISR(){// button isr to press to skip tracks 
  unsigned long interruptTime = millis(); 
  if (interruptTime - lastInterruptTime > 300){ //debouncing
    nextFlag = true; //flag set to true
  }
  lastInterruptTime = interruptTime; 
}

void pauseISR(){
  unsigned long interruptTime = millis(); 
  if (interruptTime - lastInterruptTime > 300){ //debouncing
    pauseFlag = true; //flag set to true
  }
  lastInterruptTime = interruptTime; 
}

void updateVolumeDisplay(int volume){
  lcd.setCursor(10,1); 
  lcd.print("V:"); 
  lcd.print(volume); 
  lcd.print("     "); 
}
































