#include "BLEDevice.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <WifiLocation.h>

//Command to start mosquitto: docker run -it --rm --name mosquitto -p 1883:1883 -v "$(pwd)/mosquitto/:/mosquitto/" eclipse-mosquitto


/*--- MQTT AND WIFI CONFIGURATION ---*/
const char* ssid = "wifi-id";
const char* passwd = "wifi-pasword";
const char* mqtt_server = "mqtt-server-address";
WiFiClient espClient;
PubSubClient client(espClient);

/*--- BLE CONFIGURATION ---*/
static BLEUUID serviceUUID("9e3764f5-e264-4135-a2a9-70f5b8c8330e"); // People counter service
static BLEUUID charUUID("3e715fb3-6d8d-442c-8ec6-a35ff777799c"); // Delta
static BLEUUID serviceUUID2("8ae8d0e1-946a-4d8e-92fd-949c1f04d3e7"); // Bus stop sensor
static BLEUUID charUUID2("707479d0-5a40-4775-bf9a-ed7cc3fc726e"); // Bus stop identifier
static BLEUUID charUUID3("77962ccf-b032-4467-b03f-1bde4f9bcf71"); // Start analysis


typedef struct {
  BLEAdvertisedDevice* device;
  boolean conn;
  int value;
} server;

//Servers that offer the service we are are looking for
server servers[10];
int actual_server = 0;
int connected_number = 0;
int values_number = 0;
int delta = 0;
String actual_stop = "";
static BLEAdvertisedDevice* bus_stop;
static boolean doBusStop = false;


static boolean doConnect = false;
//static boolean connected = false;
static boolean doScan = false;
static BLERemoteCharacteristic* pRemoteCharacteristic;
static BLERemoteCharacteristic* pRemoteCharacteristic2;
static BLERemoteCharacteristic* pRemoteCharacteristic3;

const char* readStopId() {
  // Reading the identifier of the actual stop
  if(pRemoteCharacteristic2->canRead()) {
    std::string value = pRemoteCharacteristic2->readValue();
    const char* val = value.c_str();
    Serial.print("The actual stop value is: ");
    Serial.println(val);
    actual_stop = val;
  }
}

void startVideoAnalysis() {
  // Starting the video analysis
  String newValue = "1";
  Serial.println("Setting new characteristic value to \"" + newValue + "\"");
  int i = 0;
  for (i=0; i<actual_server; i++) {
    if (servers[i].conn == true) {
      // Set the characteristic's value to be the array of bytes that is actually a string.
      pRemoteCharacteristic3->writeValue(newValue.c_str(), newValue.length());
    }
  } 
}

bool connectToBusStop() {
  //Connect to bus stop
  Serial.print("Forming a connection to ");
  Serial.println(bus_stop->getAddress().toString().c_str());
  
  BLEClient*  pClient  = BLEDevice::createClient();
  Serial.println(" - Created client");
  
  // Connect to the remove BLE Server.
  pClient->connect(bus_stop);  // if you pass BLEAdvertisedDevice instead of address, it will be recognized type of peer device address (public or private)
  Serial.println(" - Connected to server bus stop");

  // Obtain a reference to the service we are after in the remote BLE server.
  BLERemoteService* pRemoteService = pClient->getService(serviceUUID2);
  if (pRemoteService == nullptr) {
    Serial.print("Failed to find our service bus stop UUID: ");
    Serial.println(serviceUUID2.toString().c_str());
    pClient->disconnect();
    return false;
  }
  Serial.println(" - Found our service bus stop");

  // Obtain a reference to the characteristic in the service of the remote BLE server.
  pRemoteCharacteristic2 = pRemoteService->getCharacteristic(charUUID2);
  if (pRemoteCharacteristic2 == nullptr) {
    Serial.print("Failed to find our characteristic bus stop UUID: ");
    Serial.println(charUUID2.toString().c_str());
    pClient->disconnect();
    return false;
  }
  doBusStop = false;
  return true;
}

//Connect to the servers
bool connectToServers() {
  int i = 0;
  for (i=0; i<actual_server; i++) {
    Serial.print("Forming a connection to ");
    Serial.println(servers[i].device->getAddress().toString().c_str());
    
    BLEClient*  pClient  = BLEDevice::createClient();
    Serial.println(" - Created client");

    // Connect to the remove BLE Server.
    pClient->connect(servers[i].device);  // if you pass BLEAdvertisedDevice instead of address, it will be recognized type of peer device address (public or private)
    Serial.println(" - Connected to server");

    // Obtain a reference to the service we are after in the remote BLE server.
    BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
    if (pRemoteService == nullptr) {
      Serial.print("Failed to find our service UUID: ");
      Serial.println(serviceUUID.toString().c_str());
      pClient->disconnect();
      return false;
    }
    Serial.println(" - Found our service");


    // Obtain a reference to the characteristic in the service of the remote BLE server.
    pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
    if (pRemoteCharacteristic == nullptr) {
      Serial.print("Failed to find our characteristic UUID: ");
      Serial.println(charUUID.toString().c_str());
      pClient->disconnect();
      return false;
    }
    pRemoteCharacteristic3 = pRemoteService->getCharacteristic(charUUID3);
    if (pRemoteCharacteristic3 == nullptr) {
      Serial.print("Failed to find our characteristic UUID: ");
      Serial.println(charUUID.toString().c_str());
      pClient->disconnect();
      return false;
    }
    Serial.println(" - Found our characteristic");
    /*
    if(pRemoteCharacteristic->canNotify())
      pRemoteCharacteristic->registerForNotify(notifyCallback);
    */

    servers[i].conn = true;
    connected_number++;
  }
  return true;
}

int checkIfAlreadyPresent(BLEAdvertisedDevice* device) {
  int i = 0;
  for (i=0; i<actual_server; i++) {
    if (servers[i].device->getAddress().equals(device->getAddress())) return i;
  }
  return -1;
}

/**
 * Scan for BLE servers and find the first one that advertises the service we are looking for.
 */
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
 /**
   * Called for each advertising BLE server.
   */
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    Serial.print("BLE Advertised Device found: ");
    Serial.println(advertisedDevice.toString().c_str());

    // We have found a device, let us now see if it contains the service we are looking for.
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID)) {
      Serial.println("Found a server");
      //If the server offers this service and it is not already present, adds it to the servers array
      BLEAdvertisedDevice* device = new BLEAdvertisedDevice(advertisedDevice);
      int serverIndex = checkIfAlreadyPresent(device);
      Serial.println(serverIndex);
      if (serverIndex == -1) {
        // Saving this server
        servers[actual_server].device = device;
        servers[actual_server].conn = false;
        //-100 is the value that represent no data
        servers[actual_server].value = -100;
        actual_server = actual_server + 1;
        doConnect = true;
        doScan = true;
      }
      //Trying to reconnect this device
      else if (servers[serverIndex].conn == false) {
        doConnect = true;
        doScan = true;
      }

    } // Found our server
    // Check if we are at bus stop
    else if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID2)) {
      Serial.println("Found bus stop identifier");
      BLEAdvertisedDevice* tmp = new BLEAdvertisedDevice(advertisedDevice);
      if (bus_stop != NULL) {
        if (bus_stop->getAddress().equals(tmp->getAddress())) {
          Serial.println("Same bus stop");
          //Do nothing
        }
        else {
          Serial.println("New Bus Stop");
          bus_stop = tmp;
          doBusStop = true;
        }
      }
      else {
        bus_stop = tmp;
        doBusStop = true;
      }
    } //Found bus stop
  } // onResult
}; // MyAdvertisedDeviceCallbacks

// MQTT reconnection
void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect("WiFiClient")) {
      Serial.println("connected");
      // Subscribe
      client.subscribe("esp32/output");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting Arduino BLE Client application...");
  BLEDevice::init("");

  // Retrieve a Scanner and set the callback we want to use to be informed when we
  // have detected a new device.  Specify that we want active scanning and start the
  // scan to run for 5 seconds.
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
  pBLEScan->start(5, false);
  
  // Connect to WPA/WPA2 network
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, passwd);
  while (WiFi.status() != WL_CONNECTED) {
      Serial.print("Attempting to connect to WPA SSID: ");
      Serial.println(ssid);
      // wait 5 seconds for connection:
      Serial.print("Status = ");
      Serial.println(WiFi.status());
      delay(500);
  }
  Serial.println ("Connected");
  setClock ();

  // MQTT
  client.setServer(mqtt_server, 1883);
} // End of setup.

// Set time via NTP, as required for x.509 validation
void setClock () {
  configTime (0, 0, "pool.ntp.org", "time.nist.gov");

  Serial.print ("Waiting for NTP time sync: ");
  time_t now = time (nullptr);
  while (now < 8 * 3600 * 2) {
      delay (500);
      Serial.print (".");
      now = time (nullptr);
  }
  struct tm timeinfo;
  gmtime_r (&now, &timeinfo);
  Serial.print ("\n");
  Serial.print ("Current time: ");
  Serial.print (asctime (&timeinfo));
}

void checkIfConnected() {
  int i = 0;
  for (i=0; i<actual_server; i++) {
    if (servers[i].conn == false) doConnect = true;
  }
  doConnect = false;
}

void publishCount(const char* count) {
  Serial.print("People count: ");
  Serial.println(count);
  String deltastr = String(count);
  String json = "{\"d\":" + deltastr + ", \"s\":" + actual_stop + "}";
  client.publish("line1/bus1", json.c_str());
  int i = 0;
  //Resetting
  for (i=0; i<actual_server; i++) {
    servers[i].value = -100;
    values_number--;
  }
  delta = 0;
  values_number = 0;
}

// This is the Arduino main loop function.
void loop() {

  // If the flag "doConnect" is true then we have scanned for and found the desired
  // BLE Server with which we wish to connect.  Now we connect to it.  Once we are 
  // connected we set the connected flag to be true.
  
  if (doConnect == true) {
    if (connectToServers()) {
      Serial.println("We are now connected to the BLE Server.");
    } else {
      Serial.println("We have failed to connect to the server; there is nothin more we will do.");
    }
    doConnect = false;
  }
  else {
    
    // If the flag doConnect is false check if the client is connected to all the servers, 
    // else set the flag to true to eventually make the connection in the next iteration
    
    checkIfConnected();
  }

  if (doBusStop == true) {
    if (connectToBusStop()) {
      Serial.println("We are at a bus stop");
      readStopId();
    }
    else {
      Serial.println("We have failed to connect at a bus stop");
    }
    doBusStop = false;
    startVideoAnalysis();
  }

  // MQTT loop
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  int i = 0;
  for (i=0; i<actual_server; i++) {
    if (servers[i].conn == true) {
      
      if(pRemoteCharacteristic->canRead()) {
        std::string value = pRemoteCharacteristic->readValue();
        const char* val = value.c_str();
        Serial.print("The characteristic value is: ");
        Serial.println(val);
        int strLength = strlen(val);
        if (strLength > 0) {
          int v = atoi(val);
          // Checks if the delta is not '#', else the doors are closed
          if (val[0] != '#') {
            servers[i].value = v;
            values_number++;
            delta = delta + v;
            //publishCount(value.c_str());
          }
          //publishPositioning(loc);
        }
        else {
          //Device disconnection
          servers[i].conn = false;
          connected_number--;
        }
      }
      //When all the raspberry have communicated the value, publish the total sum 
      if (values_number == connected_number) {
        //publish
        char deltastr[8];
        itoa(delta, deltastr, 10);
        publishCount(deltastr);
        //publishCount(value.c_str());
      }
    }
    //Rescan to find new devices for 1 second
    if (i == actual_server-1) {
      BLEDevice::getScan()->start(1);
    }
  }
  
  delay(3000); // Delay 3 seconds between loops.
} // End of loop
