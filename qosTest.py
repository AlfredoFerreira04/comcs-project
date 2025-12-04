import socket
import json
import time

# --- Configuration ---
SERVER_IP = '127.0.0.1'  # Server's IP address
SERVER_PORT = 5005       # Server's UDP port
TIMEOUT = 1.0            # Socket timeout for receiving ACK

# --- Test Payload Structure ---
TEST_ID = "TestDev-QoS1"
BASE_PAYLOAD = {
    "id": TEST_ID,
    "temperature": 25.5,
    "relativeHumidity": 55.0,
    "dateObserved": time.strftime("%Y-%m-%dT%H:%M:%S"),
    "qos": 1,
    "seq": 0  # Sequence number will be updated
}

# --- Utility Functions ---

def create_socket():
    """Creates a UDP socket configured for the test."""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(TIMEOUT)
        return sock
    except socket.error as e:
        print(f"Error creating socket: {e}")
        return None

def send_and_wait_for_ack(sock, seq_num):
    """
    Constructs a packet, sends it, and attempts to receive the ACK.
    Returns the received data or None.
    """
    payload = BASE_PAYLOAD.copy()
    payload['seq'] = seq_num
    
    # Update dateObserved for realism (or to detect data change)
    payload['dateObserved'] = time.strftime("%Y-%m-%dT%H:%M:%S")

    message = json.dumps(payload).encode('utf-8')
    
    print(f"\n[CLIENT] Sending SEQ: {seq_num}...")
    try:
        # Send the data
        sock.sendto(message, (SERVER_IP, SERVER_PORT))
        
        # Wait for the ACK
        data, server = sock.recvfrom(8192)
        ack_data = json.loads(data.decode('utf-8'))
        print(f"[CLIENT] RECEIVED ACK: {ack_data}")
        return ack_data

    except socket.timeout:
        print(f"[CLIENT] ERROR: Timeout waiting for ACK (seq: {seq_num})")
        return None
    except Exception as e:
        print(f"[CLIENT] ERROR during send/recv: {e}")
        return None

def validate_ack(ack_data, expected_seq):
    """Checks if the received ACK matches the expected structure and sequence."""
    if ack_data is None:
        print("  ❌ Test Failed: No ACK received.")
        return False
        
    is_valid = (ack_data.get('type') == 'ACK' and
                ack_data.get('id') == TEST_ID and
                ack_data.get('seq') == expected_seq)
    
    if is_valid:
        print("  ✅ Validation Successful: ACK matched expectations.")
    else:
        print(f"  ❌ Validation Failed: ACK content was unexpected.")
        print(f"     Expected: type=ACK, id={TEST_ID}, seq={expected_seq}")
        print(f"     Received: {ack_data}")
        
    return is_valid

# --- Main Test Execution ---

def run_qos_tests():
    sock = create_socket()
    if not sock:
        return

    # --- SCENARIO 1: Initial Successful Receipt (ACK Test) ---
    print("\n\n--- SCENARIO 1: Initial Packet (SEQ 100) ---")
    print("Goal: Server should process data and send ACK 100.")
    seq_1 = 100
    ack_1 = send_and_wait_for_ack(sock, seq_1)
    validate_ack(ack_1, seq_1)

    # --- SCENARIO 2: Duplicate Packet Handling (Re-ACK Test) ---
    print("\n\n--- SCENARIO 2: Sending Duplicate Packet (SEQ 100 again) ---")
    print("Goal: Server should detect duplicate, re-send ACK 100, and SKIP data processing.")
    seq_2 = 100  # Duplicate sequence number
    ack_2 = send_and_wait_for_ack(sock, seq_2)
    
    # Crucial check: validate that the server STILL sends the ACK
    # (The data skip must be verified by observing the server log: "Duplicate seq ...")
    validate_ack(ack_2, seq_2)

    # --- SCENARIO 3: Next Valid Packet (New Sequence) ---
    print("\n\n--- SCENARIO 3: Next Valid Packet (SEQ 101) ---")
    print("Goal: Server should process new data and send ACK 101.")
    seq_3 = 101
    ack_3 = send_and_wait_for_ack(sock, seq_3)
    validate_ack(ack_3, seq_3)

    sock.close()
    print("\n--- QoS Tests Complete ---")

if __name__ == "__main__":
    run_qos_tests()
