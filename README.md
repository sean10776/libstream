# libstream

This is a C library providing various network and file flow operations. The library includes the following types of flows:

## Features

- **Files**: Support reading and writing files.
- **Serial Port**: Support serial read/write data.
- **TCP**: Support TCP server and client connections.
- **UDP**: Support UDP server and client connections.
- **NTRIP**: Support NTRIP protocol server and client connections.

## Usage Example

The following is a basic example of using the libstream library:

```c
#include <time.h>
#include "stream.h"

int main() {
     // Initialize common serial communication
    strinitcom();
    
    struct stream_t str;
    // Initialize flow
    strinit(&str); 

    // Open Flow
    if (stropen(&str, STR_SERIAL, STR_MODE_RW, "/dev/serial")) {        
        // Start data transmission flow
        while (1) {
            unsigned char buffer[1024];
            int n;
            
            // Read data into the flow
            nr = strread(&str, buffer, sizeof(buffer), 1024);
            
            if (nr == -1) break;    // Error
            
            sleep(1);                // Sleep for 1 second
            
            // Write data into the flow
            ns = strwrite(&str, buffer, sizeof(buffer), 1024);
            
            printf("Write %zu bytes\n", ns);
        }
        
    }
    // Close the flow
    strclose(&str);

    return 0;
}
```

## Function Overview

### Flow Types
- **STR_FILE**: Represents a file flow.
- **STR_SERIAL**: Represents a serial flow.
- **STR_TCPSVR**: Represents a TCP server flow.
- **STR_TCPCLI**: Represents a TCP client flow.
- **STR_UDP**: Represents a UDP server or client flow.
- **STR_NTRIPSVR**: Represents an NTRIP server flow.
- **STR_NTRIPCLI**: Represents an NTRIP client flow.

### Method Overview
- **Open Flow**: The `stropen()` function is used to open the required flow.
- **Close Flow**: The `strclose()` function is used to close already opened flows, releasing associated resources.
- **Read Data**: The `strread()` function reads data from the flow.
- **Write Data**: The `strwrite()` function writes data into the flow.
- **Get State**: The `strstat()` and `strstatx()` functions are used to retrieve the current state of the flow, including connection states and errors.

## TODO
- [x] File
- [x] Serial
- [x] TCP (Server/Client)
- [x] UDP (Server/Client)
- [x] NTRIP (Server/Client)
- [ ] TCP Client+TSL/SSL