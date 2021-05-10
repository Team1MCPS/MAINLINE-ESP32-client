#include "BLEDevice.h"
#include <WiFi.h>
#include <PubSubClient.h>

/*--- MQTT AND WIFI CONFIGURATION ---*/
const char* ssid = "wifi-network";
const char* passwd = "wifi-password";
const char* mqtt_server = "mqtt-broker-address";
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

// Maximum number of servers
const int servers_number = 10;
// Servers that offer the service we are are looking for
server servers[servers_number];
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

// Returns the index of the first free position in the array that stores the servers
int getFirstFreeServerIndex() {
  for (int i=0; i<servers_number; i++) {
    if (servers[i].device == NULL) return i;
  }
  return -1;
}


// Resets device information
void resetServer(server* s) {
  s->device = NULL;
  s->pRemoteCharacteristic = NULL;
  s->pRemoteCharacteristic2 = NULL;
  s->pClient = NULL;
  s->pRemoteService = NULL;
  s->conn = false;
  s->value = -100;
}


// Connects to servers
bool connectToServers() {
  for (int i=0; i<servers_number; i++) {
    if (servers[i].device != NULL && servers[i].conn == false) {
      Serial.print("Forming a connection to ");
      Serial.println(servers[i].device->getAddress().toString().c_str());
      
      bool res = servers[i].pClient  = BLEDevice::createClient();
      if (res == 1) {
        Serial.println(" - Created client");
      }
      else {
        Serial.println("Failed to create client");
        resetServer(&servers[i]);
        return false;
      }
  
      // Connect to the remote BLE Server.
      res = servers[i].pClient->connect(servers[i].device);
      if (res == 1) {
        Serial.println(" - Connected to server");
      }
      else {
        Serial.println("Failed to connect to server");
        resetServer(&servers[i]);
        return false;
      }
  
      // Obtain a reference to the service we are after in the remote BLE server.
      servers[i].pRemoteService = servers[i].pClient->getService(serviceUUID);
      if (servers[i].pRemoteService == nullptr) {
        Serial.print("Failed to find our service UUID: ");
        Serial.println(serviceUUID.toString().c_str());
        servers[i].pClient->disconnect();
        resetServer(&servers[i]);
        return false;
      }

      // Obtain references to the characteristics in the service of the remote BLE server.
      servers[i].pRemoteCharacteristic = servers[i].pRemoteService->getCharacteristic(charUUID);
      if (servers[i].pRemoteCharacteristic == nullptr) {
        Serial.print("Failed to find our characteristic UUID: ");
        Serial.println(charUUID.toString().c_str());
        servers[i].pClient->disconnect();
        resetServer(&servers[i]);
        return false;
      }
      servers[i].pRemoteCharacteristic2 = servers[i].pRemoteService->getCharacteristic(charUUID2);
      if (servers[i].pRemoteCharacteristic2 == nullptr) {
        Serial.print("Failed to find our characteristic UUID: ");
        Serial.println(charUUID2.toString().c_str());
        servers[i].pClient->disconnect();
        resetServer(&servers[i]);
        return false;
      }

      servers[i].conn = true;
      // Update number of connected servers
      connected_number++;
    }
  }
  return true;
}

// Check if a server is already saved
int checkIfAlreadyPresent(BLEAdvertisedDevice* device) {
  int i = 0;
  for (i=0; i<servers_number; i++) {
    if (servers[i].device != NULL) {
      // If this server is found return its index
      if (servers[i].device->getAddress().equals(device->getAddress())) return i;
    }
  }
  // Return -1 if the server is not already present
  return -1;
}

// Scan for the servers that offer the desired service or for detecting bus stops
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  // Called for each advertised device
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    Serial.print("BLE Advertised Device found: ");
    Serial.println(advertisedDevice.toString().c_str());
    // Found a device, see if it contains the service we are looking for.
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID)) {
      Serial.println("Found a server");
      // If the server offers this service and it is not already present, adds it to the servers array
      BLEAdvertisedDevice* device = new BLEAdvertisedDevice(advertisedDevice);
      int serverIndex = checkIfAlreadyPresent(device);
      if (serverIndex == -1) {
        int actual_server = getFirstFreeServerIndex();
        if (actual_server != -1) {
          // Saving this server
          servers[actual_server].device = device;
          servers[actual_server].conn = false;
          //-100 is the value that represents no data
          servers[actual_server].value = -100;
          doConnect = true;
        }
        else {
          Serial.println("There are too much servers at the moment");
        }
      }
    } // Found our server
    // Check if we are at bus stop
    else if (advertisedDevice.getServiceDataUUID().equals(BLEUUID(beconUUID))==true) {  // found Eddystone UUID
      std::string strServiceData = advertisedDevice.getServiceData();
      uint8_t cServiceData[100];
      strServiceData.copy((char *)cServiceData, strServiceData.length(), 0);
      // Get Service Data
      boolean found = true;
      int j = 0;
      // Check if the namespace id is what we are looking for
      for (int i=2;i<12;i++) {
        if (cServiceData[i] != stopService[j]) found = false;
        j++;
      }
      if (found == true) {
        // Get instance id to know the identifier of the bus stop
        String instanceId = "";
        for (int i=12;i<strServiceData.length();i++) {
          // Convert from hex to int
          int number = (int) cServiceData[i];
          String n = String(number);
          instanceId = instanceId + n;
        }
        int final_number = instanceId.toInt();
        instanceId = String(final_number);
        if (instanceId != actual_stop) {
          Serial.println("Found stop: " + instanceId);
          actual_stop = instanceId;
          // Starting the video analysis
          doAnalysis = true;
        }
      } //Found bus stop 
    } //Found an eddystone beacon
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

// Setup procedure
void setup() {
  Serial.begin(115200);
  Serial.println("Starting ESP32 application...");
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

  // Set MQTT server
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

// Publishes the delta for the last stop over MQTT
void publishCount(const char* count) {
  Serial.print("Delta people: ");
  Serial.println(count);
  String deltastr = String(count);
  // Transform the values in a JSON string and publish it
  String json = "{\"d\":" + deltastr + ", \"s\":" + actual_stop + "}";
  client.publish("line1/bus1", json.c_str());
  int i = 0;
  //Resetting
  for (i=0; i<servers_number; i++) {
    if (servers[i].device != NULL) {
      servers[i].value = -100;
      values_number--;
    }
  }
  delta = 0;
  values_number = 0;
}

// This is the ESP-32 main loop function.
void loop() {

  // If the flag "doConnect" is true then we have scanned for and found the desired
  // BLE servers we wish to connect with.
  if (doConnect == true) {
    connectToServers();
    doConnect = false;
  }
  
  // Communicates to the servers that we are at a stop if doAnalysis is true
  if (doAnalysis == true) {
    for (int i=0; i<servers_number; i++) {
      if (servers[i].device != NULL && servers[i].conn == true) {
        String newValue = String(1);
        // Set the characteristic's value to be the array of bytes that is actually a string.
        servers[i].pRemoteCharacteristic2->writeValue(newValue.c_str(), false);
      }
    }
    doAnalysis = false;
  }

  // MQTT loop
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  // Compute delta and publish the info when all servers have communicated it
  for (int i=0; i<servers_number; i++) {
    if (servers[i].device != NULL && servers[i].conn == true) {
      // Read the value of the characteristic.
      if(servers[i].pRemoteCharacteristic->canRead()) {
        std::string value = servers[i].pRemoteCharacteristic->readValue();
        const char* val = value.c_str();
        int strLength = strlen(val);
        if (strLength > 0) {
          // Convert value string to int
          int v = atoi(val);
          // Checks if the delta is not '#', else the value is ignored
          if (val[0] != '#') {
            Serial.print("The characteristic value is: ");
            Serial.println(val);
            servers[i].value = v;
            // Increment the number of useful values received
            values_number++;
            // Compute the delta until now
            delta = delta + v;
          }
        }
        else {
          // Device disconnection if the characteristic is empty
          resetServer(&servers[i]);
          connected_number--;
        }
      }
      // When all the connected servers have communicated the value, publish the total sum 
      if ((values_number > 0) && (values_number == connected_number)) {
        // publish
        char deltastr[8];
        itoa(delta, deltastr, 10);
        publishCount(deltastr);
      }
    }
  }
  // Rescan to find new devices for 1 second
  BLEDevice::getScan()->start(1);
  delay(1000); // Delay 1 seconds between loops.
} // End of loop