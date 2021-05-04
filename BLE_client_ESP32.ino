#include "BLEDevice.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <WifiLocation.h>

//Command to start mosquitto with a configuration file placed in ./mosquitto/config folder: 
// docker run -it --rm --name mosquitto -p 1883:1883 -v "$(pwd)/mosquitto/:/mosquitto/" eclipse-mosquitto

/*--- MQTT AND WIFI CONFIGURATION ---*/
const char* ssid = "wifi-network";
const char* passwd = "wifi-password";
const char* mqtt_server = "mqtt-server-address";
WiFiClient espClient;
PubSubClient client(espClient);

/*--- BLE CONFIGURATION ---*/
static BLEUUID serviceUUID("9e3764f5-e264-4135-a2a9-70f5b8c8330e"); // People counter service
static BLEUUID charUUID("3e715fb3-6d8d-442c-8ec6-a35ff777799c"); // Delta
static BLEUUID charUUID2("77962ccf-b032-4467-b03f-1bde4f9bcf71"); // Start analysis
static uint8_t stopService[10] = {0x94,0x29,0xAE,0xF4,0x48,0x98,0x24,0x9E,0x8F,0x6E}; // Bus stop sensors
uint16_t beconUUID = 0xFEAA; // Eddystone beacon uuid

// Struct that represents a BLE server
typedef struct {
  BLEAdvertisedDevice* device;
  BLERemoteCharacteristic* pRemoteCharacteristic;
  BLERemoteCharacteristic* pRemoteCharacteristic2;
  BLEClient*  pClient;
  BLERemoteService* pRemoteService;
  boolean conn;
  int value;
} server;

// Servers that offer the service we are are looking for
server servers[10];
// Index of the last added server
int actual_server = 0;
// Number of connected servers
int connected_number = 0;
// Number of servers that have communicated a value
int values_number = 0;
// Actual delta for the last stop
int delta = 0;
// Identifier of the last stop
String actual_stop = "";

// Flags
static boolean doConnect = false;
static boolean doAnalysis = false;


//Connect to the servers
bool connectToServers() {
  int i = 0;
  for (i=0; i<actual_server; i++) {
    if (servers[i].conn == false) {
      Serial.print("Forming a connection to ");
      Serial.println(servers[i].device->getAddress().toString().c_str());
      
      servers[i].pClient  = BLEDevice::createClient();
      Serial.println(" - Created client");
  
      // Connect to the remove BLE Server.
      servers[i].pClient->connect(servers[i].device);  // if you pass BLEAdvertisedDevice instead of address, it will be recognized type of peer device address (public or private)
      Serial.println(" - Connected to server");
  
      // Obtain a reference to the service we are after in the remote BLE server.
      servers[i].pRemoteService = servers[i].pClient->getService(serviceUUID);
      if (servers[i].pRemoteService == nullptr) {
        Serial.print("Failed to find our service UUID: ");
        Serial.println(serviceUUID.toString().c_str());
        servers[i].pClient->disconnect();
        return false;
      }
      Serial.println(" - Found our service");
      // Obtain a reference to the characteristic in the service of the remote BLE server.
      servers[i].pRemoteCharacteristic = servers[i].pRemoteService->getCharacteristic(charUUID);
      if (servers[i].pRemoteCharacteristic == nullptr) {
        Serial.print("Failed to find our characteristic UUID: ");
        Serial.println(charUUID.toString().c_str());
        servers[i].pClient->disconnect();
        return false;
      }
      servers[i].pRemoteCharacteristic2 = servers[i].pRemoteService->getCharacteristic(charUUID2);
      if (servers[i].pRemoteCharacteristic2 == nullptr) {
        Serial.print("Failed to find our characteristic UUID: ");
        Serial.println(charUUID2.toString().c_str());
        servers[i].pClient->disconnect();
        return false;
      }
      Serial.println(" - Found our characteristics");
      servers[i].conn = true;
      connected_number++;
    }
  }
  return true;
}

// Check if a server is already saved
int checkIfAlreadyPresent(BLEAdvertisedDevice* device) {
  int i = 0;
  for (i=0; i<actual_server; i++) {
    if (servers[i].device->getAddress().equals(device->getAddress())) return i;
  }
  return -1;
}

// Scan for the servers that offer the desired service or if we are at a bus stop
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  // Called for each advertised device
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
      }
      //Trying to reconnect this device
      else if (servers[serverIndex].conn == false) {
        doConnect = true;
      }

    } // Found our server
    // Check if we are at bus stop
    else if (advertisedDevice.getServiceDataUUID().equals(BLEUUID(beconUUID))==true) {  // found Eddystone UUID
      std::string strServiceData = advertisedDevice.getServiceData();
      uint8_t cServiceData[100];
      strServiceData.copy((char *)cServiceData, strServiceData.length(), 0);
      //Serial.printf("Eddystone: %d %s length %d\n", advertisedDevice.getServiceDataUUID().bitSize(), advertisedDevice.getServiceDataUUID().toString().c_str(),strServiceData.length());
      //Get Service Data
      boolean found = true;
      int j = 0;
      for (int i=2;i<12;i++) {
        if (cServiceData[i] != stopService[j]) found = false;
        j++;
      }
      if (found == true) {
        Serial.println("Found a bus stop");
        String id = "";
        // Instance id to know the identifier of the bus stop
        String instanceId = "";
        for (int i=2;i<12;i++) {
          int number = cServiceData[i];
          String n = String(number);
          id = id + n;
        }
        for (int i=13;i<=strServiceData.length();i++) {
          int number = cServiceData[i];
          String n = String(number);
          instanceId = instanceId + n;
        }
        Serial.println("Actual stop: " + instanceId);
        if (instanceId != actual_stop) {
          actual_stop = instanceId;
          // Starting the video analysis
          doAnalysis = true;
        }
      }//Found bus stop 
    } //Found eddystone beacon
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
  //This is for received messages, not this case
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

// Publish the delta for the last stop over MQTT
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
  // Communicates to the servers that we are at a stop
  if (doAnalysis == true) {
    for (int i=0; i<actual_server; i++) {
      if (servers[i].conn == true) {
        String newValue = String(servers[i].value);
        Serial.println("Setting new characteristic value to \"" + newValue + "\"");
        // Set the characteristic's value to be the array of bytes that is actually a string.
        servers[i].pRemoteCharacteristic2->writeValue(newValue.c_str(), newValue.length());
      }
    }
    doAnalysis = false;
  }

  // MQTT loop
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  // Compute delta and publish when all servers have communicated it
  for (int i=0; i<actual_server; i++) {
    if (servers[i].conn == true) {
      // Read the value of the characteristic.
      if(servers[i].pRemoteCharacteristic->canRead()) {
        std::string value = servers[i].pRemoteCharacteristic->readValue();
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
          }
        }
        else {
          //Device disconnection if the characteristic is empty
          servers[i].conn = false;
          connected_number--;
        }
      }
      //When all the servers have communicated the value, publish the total sum 
      if (values_number == connected_number) {
        //publish
        char deltastr[8];
        itoa(delta, deltastr, 10);
        publishCount(deltastr);
        //publishCount(value.c_str());
      }
    }
  }
  //Rescan to find new devices for 1 second
  BLEDevice::getScan()->start(1);
  delay(3000); // Delay 3 seconds between loops.
} // End of loop
