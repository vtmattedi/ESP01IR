import paho.mqtt.client as mqtt
import time

MQTT_URL = "64000315ae8845b9b763275cb3d59b8c.s2.eu.hivemq.cloud"
MQTT_PORT = 8883
MQTT_USER = "Turing"
MQTT_PASSWD = "5Ep3ub@u4LV9u6t"

# Callback when a connection to the broker is established
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("Connected to MQTT broker")
    else:
        print(f"Failed to connect, result code: {rc}")

# Callback when a message is published
def on_publish(client, userdata, mid):
    print("Message published")

# Callback when a message is received
def on_message(client, userdata, msg):
    print(f"Received message: {msg.payload.decode()}")

# Configure the client
client = mqtt.Client("PythonClient")
client.username_pw_set(username=MQTT_USER, password=MQTT_PASSWD)  # Replace with your credentials
client.on_connect = on_connect
client.on_publish = on_publish
client.on_message = on_message

# Connect to the broker
broker_address = MQTT_URL  # Replace with your broker's address
a = client.connect(broker_address, MQTT_PORT)
print(a)

# Loop to maintain connection and process callbacks
client.loop_start()

# Publish a message
topic = "Adler/Shell"
message = "Hello from Python MQTT client!"
client.publish(topic, message)

# Subscribe to a topic
client.subscribe("Adler/#")

while True:

# Wait for a few seconds to receive messages
    time.sleep(1)

# Disconnect
client.loop_stop()
client.disconnect()
