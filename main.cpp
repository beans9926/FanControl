#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include "libloaderapi.h"
#include "nvml.h"
#include <Windows.h>
#include <stdexcept>
#include <thread>
//#include <chrono>
#include <atomic>
#include <csignal>

std::atomic<bool> stopRequest(false);

void signalHandler(int signalNum){
    stopRequest = true;
}

typedef void (__stdcall* lpWriteByte)(short, unsigned char);
typedef unsigned char (__stdcall* lpReadByte)(short);
typedef BOOL (__stdcall* lpIsDriverOpen)(void);

//Standard acpi ec ports
constexpr int EC_CMD_PORT = 0x66;
constexpr int EC_DATA_PORT = 0x62;

//acpi commands
constexpr int EC_CMD_READ = 0x80;
constexpr int EC_CMD_WRITE = 0x81;

//status register flags
constexpr int EC_IBF = 0x02;
constexpr int EC_OBF = 0x01;

class EcController {
private:
    HMODULE hInpOutDll;
    lpWriteByte writePort;
    lpReadByte readPort;
    lpIsDriverOpen IsDriverOpen;

    // Helper: Wait for Input Buffer Empty
    void WaitIBF() {
        for (int i = 0; i < 10000; i++) {
            // Read status from 0x66
            if (!(readPort(EC_CMD_PORT) & EC_IBF)) return;
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

public:
    EcController() : hInpOutDll(nullptr), writePort(nullptr), readPort(nullptr) {
        hInpOutDll = LoadLibraryA(TEXT("inpoutx64.dll"));
        if (hInpOutDll == NULL) throw std::runtime_error("Failed to load inpoutx64.dll");

        // Use the Uchar versions for safety!
        writePort = (lpWriteByte)GetProcAddress(hInpOutDll, "DlPortWritePortUchar");
        readPort = (lpReadByte)GetProcAddress(hInpOutDll, "DlPortReadPortUchar");

        // FIXED TYPO HERE
        IsDriverOpen = (lpIsDriverOpen)GetProcAddress(hInpOutDll, "IsInpOutDriverOpen");

        if (!writePort || !readPort || !IsDriverOpen) {
            throw std::runtime_error("Failed to map DLL functions. Check names.");
        }

        if (!IsDriverOpen()) {
            throw std::runtime_error("Driver not open. Run as Administrator.");
        }
    }

    ~EcController() {
        if (hInpOutDll) FreeLibrary(hInpOutDll);
    }


        // Spinlock: Wait until the EC has put data in the buffer for us
        // We check port 0x66 bit 0 (OBF). If it's 1, data is ready.
        void WaitOBF() {
            for (int i = 0; i < 10000; i++) {
                short status = readPort(EC_CMD_PORT);
                if (status & EC_OBF) {
                    return; // Data ready
                }
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
            std::cerr << "Warning: EC Read Timeout (OBF stuck)" << std::endl;
        }

        // Read a byte from the EC RAM at specific offset
        int ReadEc(int offset) {
            WaitIBF();
            writePort(EC_CMD_PORT, EC_CMD_READ); // Tell EC we want to Read

            WaitIBF();
            writePort(EC_DATA_PORT, offset);     // Tell EC which address we want

            WaitOBF();                       // Wait for EC to fetch the data
            return readPort(EC_DATA_PORT);      // Grab the data
        }


    void WriteEC(int offset, int value) {
        WaitIBF(); // Wait before command
        writePort(EC_CMD_PORT, EC_CMD_WRITE);

        WaitIBF(); // Wait before address
        writePort(EC_DATA_PORT, (unsigned char)offset);

        WaitIBF(); // Wait before data
        writePort(EC_DATA_PORT, value); // Actually use the value!
    }
};

constexpr int GPU_FAN_WRITE_OFFSET = 0x3A;
constexpr int CPU_FAN_WRITE_OFFSET = 0X37;


int main() {
    std::signal(SIGINT, signalHandler);
    nvmlReturn_t result;
    unsigned int cpuTemp;
    unsigned int gpuTemp;
    int temporaryTemperatureCalc = 0;
    int previousCpuTemp = 0, previousGpuTemp = 0;
    EcController ec;

    ec.WriteEC(0x22, 12);
    ec.WriteEC(0x21, 48);
    //int cpuCritStartTime = 0, cpuCritCurrentTime = 0;
    //int gpuCritStartTime = 0, gpuCritCurrentTime = 0;

    // 1. Initialize NVML (Load the library)
    result = nvmlInit();
    if (NVML_SUCCESS != result) {
        std::cerr << "Failed to init NVML: " << nvmlErrorString(result) << std::endl;
        return 1;
    }

    // 2. Get the handle for the first GPU (Index 0)
    nvmlDevice_t device;
    result = nvmlDeviceGetHandleByIndex(0, &device);
    if (NVML_SUCCESS != result) {
        std::cerr << "Failed to get device: " << nvmlErrorString(result) << std::endl;
        nvmlShutdown();
        return 1;
    }

    std::cout << "NVML Init Successful. Monitoring..." << std::endl;

    while (!stopRequest) {
        system("cls");
        std::cerr << "--------------------------------------------------\n";

        // 3. Direct Query (No string parsing, just a uint)
        result = nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &gpuTemp);

        cpuTemp = ec.ReadEc(0xB1);
        int temporaryTemperatureCalc = cpuTemp - previousCpuTemp;
        if((cpuTemp && cpuTemp <= 150)&&(temporaryTemperatureCalc <= 30 || temporaryTemperatureCalc >= -20)){
            std::cerr << "CPU Temp: " << cpuTemp << " C\n";
            if(cpuTemp <= 50){ec.WriteEC(CPU_FAN_WRITE_OFFSET, 0);}
            else if(cpuTemp <= 60){
                ec.WriteEC(CPU_FAN_WRITE_OFFSET, (2 * cpuTemp) - 50);
                std::cout << "cpu fans - standard";
            }
            else{
                //ec.WriteEC(CPU_FAN_WRITE_OFFSET, (cpuTemp - 15));
                //ec.WriteEC(CPU_FAN_WRITE_OFFSET, (0.8 * cpuTemp));
                cpuTemp-=35;
                ec.WriteEC(CPU_FAN_WRITE_OFFSET, std::sqrt(cpuTemp/0.0065));
                std::cout << "data on cpu fan offset = " << ec.ReadEc(CPU_FAN_WRITE_OFFSET) << "\n";
            }
            std::cerr << "\n" << previousCpuTemp;
            previousCpuTemp = cpuTemp;
            std::cerr << " " << cpuTemp << "\n";
        }
        else{}

        //system("cls");
        if (NVML_SUCCESS == result) {
            int temporaryTemperatureCalc = gpuTemp - previousGpuTemp;
            if(temporaryTemperatureCalc <= 30 || temporaryTemperatureCalc >= -20){
                std::cout << "\nGPU Temp: " << gpuTemp << " C" << std::endl;
                if(gpuTemp <= 50){ec.WriteEC(GPU_FAN_WRITE_OFFSET, 0);}
                else if(gpuTemp <= 70){
                    ec.WriteEC(GPU_FAN_WRITE_OFFSET, (2.8 * gpuTemp) - 110);
                    std::cerr << "gpu fans - standard";
                }
                else {
                    //ec.WriteEC(GPU_FAN_WRITE_OFFSET, (gpuTemp-15));
                    //ec.WriteEC(GPU_FAN_WRITE_OFFSET, (0.8 * gpuTemp));
                    gpuTemp -= 25;
                    ec.WriteEC(GPU_FAN_WRITE_OFFSET, std::sqrt(gpuTemp/0.0055));
                    std::cout << "data on gpu fan offset = " << ec.ReadEc(GPU_FAN_WRITE_OFFSET) << "\n";
                }
                std::cerr << "\n" << previousGpuTemp;
                previousGpuTemp = gpuTemp;
                std::cerr << " " << gpuTemp << "\n";
            }
            else{}
        }
        else {
            std::cerr << "Query failed: " << nvmlErrorString(result) << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // 4. Shutdown
    std::cerr << "\nctrl+c detected, executing functions";
    ec.WriteEC(0x22, 4);
    ec.WriteEC(0x21, 16);
    nvmlShutdown();
    return 0;
}
