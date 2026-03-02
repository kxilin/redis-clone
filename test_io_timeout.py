import socket
import time

s = socket.socket()
s.connect(('127.0.0.1', 1234))
s.send(b'\x10\x00\x00\x00')  # send a 4-byte header saying "body is 16 bytes"
# but never send the body
time.sleep(5)  # wait and watch the server logs
