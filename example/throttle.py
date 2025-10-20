import time

try:
    a = 1
    while True:
        a = a + 2
        if a == 10000:
            a = 1

except Exception as e:
    print("Shutdown: ", e)

finally:
    pass
