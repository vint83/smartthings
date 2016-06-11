#include <SPI.h>
#include <Ethernet.h>
#include <sha256.h>
#include <OneWire.h>
#include <DallasTemperature.h>

//#define TESTING
//#define DEBUG

#ifdef DEBUG
    #define DEBUG_PRINT(x)  Serial.println(x)
#else
    #define DEBUG_PRINT(x)
#endif

// size of buffer used to capture HTTP requests
#define REQ_BUF_SZ   100
#define OPEN   "open"
#define CLOSE   "closed"
#define OPENING   "opening"
#define CLOSING   "closing"
#define UNKNOWN   "unknown"

// Pins:
#define closePin 23
#define openPin 25
#define relayPin 22
// Data wire is plugged into pin 2 on the Arduino
#define ONE_WIRE_BUS 43

// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(ONE_WIRE_BUS);

// Pass our oneWire reference to Dallas Temperature. 
DallasTemperature tempSensor(&oneWire);

const char password[] = "yourPassword";
volatile int current_nonce = 10000;

// MAC address from Ethernet shield sticker under board
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(192, 168, 0, 10); // IP address, may need to change depending on network

EthernetServer server(123);  // create a server at port 333

// Smartthings hub information
IPAddress hubIp(192,168,0,50); // smartthings hub ip
const unsigned int hubPort = 39500; // smartthings hub port

char readString[REQ_BUF_SZ] = {0}; // buffered HTTP request stored as null terminated string
char req_index = 0;              // index into HTTP_req buffer

// initial sensor status:
float prev_temperature = 0;
String prev_door_state = UNKNOWN;
volatile byte openSensorState = LOW;
volatile byte closeSensorState = LOW;

// Setup
void setup()
{
    #ifdef DEBUG
        Serial.begin(9600);
    #endif
    
    // disable Ethernet chip
    pinMode(10, OUTPUT);
    digitalWrite(10, HIGH);
    //delay(1000);
    //digitalWrite(10, LOW);
    
    // initialize pins
    pinMode(closePin, INPUT_PULLUP);
    pinMode(openPin, INPUT_PULLUP);
    pinMode(relayPin, OUTPUT);
    
    delay(1000);
    Ethernet.begin(mac, ip);  // initialize Ethernet device
    server.begin();           // start to listen for clients
    delay(100);
    DEBUG_PRINT(Ethernet.localIP());
    
    tempSensor.begin();
}

void getHashHEX(uint8_t* hash, char *hash_hex) {
    int i;
    char *ap = hash_hex;
    for (i=0; i<32; i++) {
        ap += sprintf(ap, "%02x", hash[i]);
    }
}

int getNonce() {
    return current_nonce;
}

boolean authenticate(char *received_hash)
{
    char expected_hash[65];
    DEBUG_PRINT(received_hash);
    char buffer[6+sizeof(password)];
    sprintf(buffer, "%d%s", current_nonce, password);
    current_nonce += 1;
    Sha256.init();
    Sha256.print(buffer);
    uint8_t *hash = Sha256.result();
    DEBUG_PRINT("calculated sha:");
    getHashHEX(hash, expected_hash);
    DEBUG_PRINT(expected_hash);
    // compare hashes
    if (strcmp(expected_hash, received_hash) == 0) {
        return true;
    }
    return false;
}

// close door
byte doorClose(char *received_hash)
{
    if (authenticate(received_hash)) {
        String state = "";
        state = getDoorState();
        if (state == OPEN or state == OPENING) {
            relayPulse();
            DEBUG_PRINT("door closed");
            return 1;
        } else {
            DEBUG_PRINT("Door was already closed. Doing nothing.");
            return 2;
        }
    } else {
        DEBUG_PRINT("Wrong password!");
        //TODO: Push notify of wrong password
        return 0;
    }
}

// open door
byte doorOpen(char *received_hash)
{
    if (authenticate(received_hash)) {
        String state = "";
        state = getDoorState();
        if (state == CLOSE or state == CLOSING) {
            relayPulse();
            DEBUG_PRINT("door opened");
            return 1;
        } else {
            DEBUG_PRINT("Door was already open. Doing nothing.");
            return 2;
        }
    } else {
        DEBUG_PRINT("Wrong password!");
        //TODO: Push notify of wrong password
        return 0;
    }
}

// triger relay for 1 sec
void relayPulse()
{
    digitalWrite(relayPin, HIGH);
    delay(1000);
    digitalWrite(relayPin, LOW);
}

// get the door state
String getDoorState()
{
    #ifdef TESTING
        // testing stuff
        //openSensorState = random(2);
        //closeSensorState = random(2);
        openSensorState = HIGH;
        closeSensorState = LOW;
    #else
        openSensorState = digitalRead(openPin);
        closeSensorState = digitalRead(closePin);
    #endif
    
    //DEBUG_PRINT(F("open:"));
    //DEBUG_PRINT(openSensorState);
    //DEBUG_PRINT(F("close:"));
    //DEBUG_PRINT(closeSensorState);
    // is door open?
    if (closeSensorState == HIGH and openSensorState == LOW) {
        //prev_door_state = OPEN;
        return OPEN;
    } // is door closed?
    else if (closeSensorState == LOW and openSensorState == HIGH){
        //prev_door_state = CLOSE;
        return CLOSE;
    }// is door in transition?
    else if (closeSensorState == HIGH and openSensorState == HIGH){
        if (prev_door_state == CLOSE or prev_door_state == OPENING) {
            //prev_door_state = OPENING;
            return OPENING;
        } else if (prev_door_state == OPEN or prev_door_state == CLOSING) {
            //prev_door_state = CLOSING;
            return CLOSING;
        } else {
            //prev_door_state = UNKNOWN;
            return UNKNOWN;
        }
    } // both low, something is bad
    else{
        //prev_door_state = UNKNOWN;
        return UNKNOWN;
    }
}

// door state changed?
boolean doorStateChanged() {
    if (prev_door_state != getDoorState()) {
        //prev_door_state = getDoorState()
        return true;
    }
    return false;
}

// handle request
void handleRequest(EthernetClient client)
{
    boolean currentLineIsBlank = true;
    while (client.connected()) {
        if (client.available()) {
            char c = client.read();
            //read char by char HTTP request
            if (req_index < (REQ_BUF_SZ - 1)) {
                readString[req_index] = c;          // save HTTP request character
                req_index++;
            }
            if (c == '\n' && currentLineIsBlank) {
                //now output HTML data header
                if(strstr(readString, "/getstatus") != NULL) {
                    client.println("HTTP/1.1 200 OK"); //send new page
                    //client.println("Connection: Keep-Alive");
                    sendDoorStateJSON(client);
                } else if (strstr(readString, "door/close") != NULL) {
                    client.println("HTTP/1.1 200 OK"); //send new page
                    //client.println("Connection: Keep-Alive");
                    char hash[65];
                    snprintf(hash, sizeof(hash), "%s", strchr(readString, '&')+1);
                    doorClose(hash);
                    sendDoorStateJSON(client);
                } else if (strstr(readString, "door/open") != NULL) {
                    client.println("HTTP/1.1 200 OK"); //send new page
                    //client.println("Connection: Keep-Alive");
                    char hash[65];
                    snprintf(hash, sizeof(hash), "%s", strchr(readString, '&')+1);
                    doorOpen(hash);
                    sendDoorStateJSON(client);
                } else {
                    client.println(F("HTTP/1.1 204 No Content"));
                    client.println();
                    client.println();
                }
                break;
            }
            if (c == '\n') {
                // you're starting a new line
                currentLineIsBlank = true;
            } else if (c != '\r') {
                // you've gotten a character on the current line
                currentLineIsBlank = false;
            }
        }
    }
    req_index = 0;
    memset(readString, 0, sizeof(readString));
    delay(1);
    //stopping client
    client.stop();
}


// send data 
int sendNotify(EthernetClient client) //client function to send/receieve POST data.
{
    DEBUG_PRINT("notify");
    int returnStatus = 1;
    if (client.connect(hubIp, hubPort)) {
        client.println(F("POST / HTTP/1.1"));
        client.print(F("HOST: "));
        client.print(hubIp);
        client.print(F(":"));
        client.println(hubPort);
        sendDoorStateJSON(client);
        DEBUG_PRINT("good");
    }
    else {
        //connection failed");
        returnStatus = 0;
        DEBUG_PRINT("bad");
    }
    
    boolean currentLineIsBlank = true;
    while (client.connected()) {
        if (client.available()) {
            char c = client.read();
            //read char by char HTTP request
            if (req_index < (REQ_BUF_SZ - 1)) {
                readString[req_index] = c;          // save HTTP request character
                req_index++;
            }
            if (c == '\n' && currentLineIsBlank) {
                //now output HTML data header
                DEBUG_PRINT(readString);
                break;
            }
            if (c == '\n') {
                // you're starting a new line
                currentLineIsBlank = true;
            } else if (c != '\r') {
                // you've gotten a character on the current line
                currentLineIsBlank = false;
            }
        }
    }
    req_index = 0;
    memset(readString, 0, sizeof(readString));
    delay(1);
    //stopping client
    client.stop();
    return returnStatus;
}


// send json data to client connection
void sendDoorStateJSON(EthernetClient client) {
  client.println(F("Content-Type: application/json"));
  //client.println(F("Content-Length: 100"));
  client.println();
  client.print("{\"door\":{\"name\":\"garage\",\"status\":\"");
  client.print(getDoorState());
  client.println("\"},\"nonce\":");
  client.print(current_nonce);
  client.println(",\"temperature\":\"");
  client.print(String(getTemperature(),1));
  client.println("\"}");
  prev_door_state = getDoorState();
}

// get temperature
float getTemperature() {
    tempSensor.requestTemperatures(); // Send the command to get temperatures
    return tempSensor.getTempFByIndex(0);
}

// == MAIN LOOP ==

void loop()
{
    EthernetClient client = server.available();
    
    if(doorStateChanged()) {
        int retries = 0;
        while (!sendNotify(client) && retries < 3) {
            delay(10);
            //sendNotify(client);
            // update old garage door state after we’ve sent the notify
            prev_door_state = getDoorState();
            retries += 1;
        }
    }
    
    if ( fabs(getTemperature() - prev_temperature) >= 1 ) {
        sendNotify(client);
        prev_temperature = getTemperature();
        DEBUG_PRINT("Temperature is: " + String(getTemperature(), 1) + " F");
    }
    
    // try to get client
    if (client) {
        handleRequest(client);
    }
    
    //delay(1000);
    
}



