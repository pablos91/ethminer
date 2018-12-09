/*
This file is part of ethminer.

ethminer is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

ethminer is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with ethminer.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <libethcore/Farm.h>
#include "CUDAMiner.h"
#include "CUDAMiner_kernel.h"
#include <nvrtc.h>

#include <ethash/ethash.hpp>

#include "CUDAMiner.h"

using namespace std;
using namespace dev;
using namespace eth;

struct CUDAChannel : public LogChannel
{
    static const char* name() { return EthOrange "cu"; }
    static const int verbosity = 2;
};
#define cudalog clog(CUDAChannel)

CUDAMiner::CUDAMiner(unsigned _index)
  : Miner("cuda-", _index),
    m_batch_size(s_gridSize * s_blockSize),
    m_streams_batch_size(s_gridSize * s_blockSize * s_numStreams)
{}

CUDAMiner::~CUDAMiner()
{
    DEV_BUILD_LOG_PROGRAMFLOW(cudalog, "cuda-" << m_index << " CUDAMiner::~CUDAMiner() begin");
    stopWorking();
    kick_miner();
    DEV_BUILD_LOG_PROGRAMFLOW(cudalog, "cuda-" << m_index << " CUDAMiner::~CUDAMiner() end");
}

unsigned CUDAMiner::s_parallelHash = 4;
unsigned CUDAMiner::s_blockSize = CUDAMiner::c_defaultBlockSize;
unsigned CUDAMiner::s_gridSize = CUDAMiner::c_defaultGridSize;
unsigned CUDAMiner::s_numStreams = CUDAMiner::c_defaultNumStreams;
unsigned CUDAMiner::s_scheduleFlag = 0;

bool CUDAMiner::initDevice()
{
    cudalog << "Using Pci Id : " << m_deviceDescriptor.UniqueId << " " << m_deviceDescriptor.cuName
            << " (Compute " + m_deviceDescriptor.cuCompute + ") Memory : "
            << dev::getFormattedMemory((double)m_deviceDescriptor.TotalMemory);

    // Set Hardware Monitor Info
    m_hwmoninfo.deviceType = HwMonitorInfoType::NVIDIA;
    m_hwmoninfo.devicePciId = m_deviceDescriptor.UniqueId;
    m_hwmoninfo.deviceIndex = -1;  // Will be later on mapped by nvml (see Farm() constructor)

    try
    {
        CUDA_SAFE_CALL(cudaSetDevice(m_deviceDescriptor.cuDeviceIndex));
        CUDA_SAFE_CALL(cudaDeviceReset());
    }
    catch (const cuda_runtime_error& ec)
    {
        cudalog << "Could not set CUDA device on Pci Id " << m_deviceDescriptor.UniqueId
                << " Error : " << ec.what();
        cudalog << "Mining aborted on this device.";
        return false;
    }
    return true;
}

bool CUDAMiner::initEpoch_internal(uint64_t block_number)
{
    // If we get here it means epoch has changed so it's not necessary
    // to check again dag sizes. They're changed for sure
    bool retVar = false;
    m_current_target = 0;
    auto startInit = std::chrono::steady_clock::now();
    size_t RequiredMemory = (m_epochContext.dagSize + m_epochContext.lightSize);

    // Release the pause flag if any
    resume(MinerPauseEnum::PauseDueToInsufficientMemory);
    resume(MinerPauseEnum::PauseDueToInitEpochError);

    try
    {
        hash64_t* dag;
        hash64_t* light;

        // If we have already enough memory allocated, we just have to
        // copy light_cache and regenerate the DAG
        if (m_allocated_memory_dag < m_epochContext.dagSize ||
            m_allocated_memory_light_cache < m_epochContext.lightSize)
        {
            // We need to reset the device and (re)create the dag
            // cudaDeviceReset() frees all previous allocated memory
            CUDA_SAFE_CALL(cudaDeviceReset());
            CUDA_SAFE_CALL(cudaSetDeviceFlags(s_scheduleFlag));
            CUDA_SAFE_CALL(cudaDeviceSetCacheConfig(cudaFuncCachePreferL1));

            CUdevice device;
            CUcontext context;
            cuDeviceGet(&device, m_deviceDescriptor.cuDeviceIndex);
            cuCtxCreate(&context, s_scheduleFlag, device);

            // Check whether the current device has sufficient memory every time we recreate the dag
            if (m_deviceDescriptor.TotalMemory < RequiredMemory)
            {
                cudalog << "Epoch " << m_epochContext.epochNumber << " requires "
                        << dev::getFormattedMemory((double)RequiredMemory) << " memory.";
                cudalog << "This device hasn't available. Mining suspended ...";
                pause(MinerPauseEnum::PauseDueToInsufficientMemory);
                return true;  // This will prevent to exit the thread and
                              // Eventually resume mining when changing coin or epoch (NiceHash)
            }

            cudalog << "Generating DAG + Light : "
                    << dev::getFormattedMemory((double)RequiredMemory);

            // create buffer for cache
            CUDA_SAFE_CALL(cudaMalloc(reinterpret_cast<void**>(&light), m_epochContext.lightSize));
            m_allocated_memory_light_cache = m_epochContext.lightSize;
            CUDA_SAFE_CALL(cudaMalloc(reinterpret_cast<void**>(&dag), m_epochContext.dagSize));
            m_allocated_memory_dag = m_epochContext.dagSize;

            // create mining buffers
            for (unsigned i = 0; i != s_numStreams; ++i)
            {
                CUDA_SAFE_CALL(cudaMallocHost(&m_search_buf[i], sizeof(Search_results)));
                CUDA_SAFE_CALL(cudaStreamCreateWithFlags(&m_streams[i], cudaStreamNonBlocking));
            }
        }
        else
        {
            cudalog << "Generating DAG + Light (reusing buffers): "
                    << dev::getFormattedMemory((double)RequiredMemory);
            get_constants(&dag, NULL, &light, NULL);
        }

        uint64_t period_seed = block_number / PROGPOW_PERIOD;
        compileKernel(period_seed, m_epochContext.dagNumItems);

        CUDA_SAFE_CALL(cudaMemcpy(reinterpret_cast<void*>(light), m_epochContext.lightCache,
            m_epochContext.lightSize, cudaMemcpyHostToDevice));

        set_constants(dag, m_epochContext.dagNumItems, light,
            m_epochContext.lightNumItems);  // in ethash_cuda_miner_kernel.cu

        ethash_generate_dag(dag, m_epochContext.dagSize, light, m_epochContext.lightNumItems, s_gridSize, s_blockSize, m_streams[0], m_deviceDescriptor.cuDeviceIndex);

        cudalog << "Generated DAG + Light in "
                << std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - startInit)
                       .count()
                << " ms. "
                << dev::getFormattedMemory((double)(m_deviceDescriptor.TotalMemory - RequiredMemory))
                << " left.";

        retVar = true;
    }
    catch (const cuda_runtime_error& ec)
    {
        cudalog << "Unexpected error " << ec.what() << " on CUDA device "
                << m_deviceDescriptor.UniqueId;
        cudalog << "Mining suspended ...";
        pause(MinerPauseEnum::PauseDueToInitEpochError);
        retVar = true;
    }
    catch (std::runtime_error const& _e)
    {
        cwarn << "Fatal GPU error: " << _e.what();
        cwarn << "Terminating.";
        exit(-1);
    }

    return retVar;
}

void CUDAMiner::workLoop()
{
    WorkPackage current;
    current.header = h256();
    uint64_t old_period_seed = -1;

    m_search_buf.resize(s_numStreams);
    m_streams.resize(s_numStreams);

    if (!initDevice())
        return;

    try
    {
        while (!shouldStop())
        {
            // Wait for work or 3 seconds (whichever the first)
            const WorkPackage w = work();
            uint64_t period_seed = w.height / PROGPOW_PERIOD;
            if (!w)
            {
                boost::system_time const timeout =
                    boost::get_system_time() + boost::posix_time::seconds(3);
                boost::mutex::scoped_lock l(x_work);
                m_new_work_signal.timed_wait(l, timeout);
                continue;
            }

            // Epoch change ?
            if (current.epoch != w.epoch)
            {
                if (!initEpoch(w.height))
                    break;  // This will simply exit the thread

                // As DAG generation takes a while we need to
                // ensure we're on latest job, not on the one
                // which triggered the epoch change
                current = w;
                continue;
            }
            else if (old_period_seed != period_seed)
            {
                const auto& context = ethash::get_global_epoch_context(w.epoch);
                const auto dagNumItems = context.full_dataset_num_items;
                const auto dagElms = dagNumItems / 2;
                compileKernel(period_seed, dagElms);
            }
            old_period_seed = period_seed;

            // Persist most recent job.
            // Job's differences should be handled at higher level
            current = w;
            uint64_t upper64OfBoundary = (uint64_t)(u64)((u256)current.boundary >> 192);

            // Eventually start searching
            search(current.header.data(), upper64OfBoundary, current.startNonce, w);
        }

        // Reset miner and stop working
        CUDA_SAFE_CALL(cudaDeviceReset());
    }
    catch (cuda_runtime_error const& _e)
    {
        string _what = "GPU error: ";
        _what.append(_e.what());
        throw std::runtime_error(_what);
    }
}

void CUDAMiner::kick_miner()
{
    m_new_work.store(true, std::memory_order_relaxed);
    m_new_work_signal.notify_one();
}

unsigned CUDAMiner::getNumDevices()
{
    int deviceCount;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    if (err == cudaSuccess)
        return deviceCount;

    if (err == cudaErrorInsufficientDriver)
    {
        int driverVersion = 0;
        cudaDriverGetVersion(&driverVersion);
        if (driverVersion == 0)
            std::cerr << "CUDA Error : No CUDA driver found" << std::endl;
        else
            std::cerr << "CUDA Error : Insufficient CUDA driver " << std::to_string(driverVersion)
                      << std::endl;
    }
    else
    {
        std::cerr << "CUDA Error : " << cudaGetErrorString(err) << std::endl;
    }

    return 0;
}

void CUDAMiner::enumDevices(std::map<string, DeviceDescriptorType>& _DevicesCollection)
{
    int numDevices = getNumDevices();
    if (numDevices)
    {
        for (int i = 0; i < numDevices; i++)
        {
            string uniqueId;
            ostringstream s;
            DeviceDescriptorType deviceDescriptor;
            cudaDeviceProp props;

            try
            {
                CUDA_SAFE_CALL(cudaGetDeviceProperties(&props, i));
                s << setw(2) << setfill('0') << hex << props.pciBusID << ":" << setw(2)
                  << props.pciDeviceID << ".0";
                uniqueId = s.str();

                if (_DevicesCollection.find(uniqueId) != _DevicesCollection.end())
                    deviceDescriptor = _DevicesCollection[uniqueId];
                else
                    deviceDescriptor = DeviceDescriptorType();

                deviceDescriptor.Name = string(props.name);
                deviceDescriptor.cuDetected = true;
                deviceDescriptor.UniqueId = uniqueId;
                deviceDescriptor.Type = DeviceTypeEnum::Gpu;
                deviceDescriptor.cuDeviceIndex = i;
                deviceDescriptor.cuDeviceOrdinal = i;
                deviceDescriptor.cuName = string(props.name);
                deviceDescriptor.TotalMemory = props.totalGlobalMem;
                deviceDescriptor.cuCompute =
                    (to_string(props.major) + "." + to_string(props.minor));
                deviceDescriptor.cuComputeMajor = props.major;
                deviceDescriptor.cuComputeMinor = props.minor;

                _DevicesCollection[uniqueId] = deviceDescriptor;
            }
            catch (const cuda_runtime_error& _e)
            {
                std::cerr << _e.what() << std::endl;
            }
        }
    }
}

unsigned const CUDAMiner::c_defaultBlockSize = 512;
unsigned const CUDAMiner::c_defaultGridSize = 1024; // * CL_DEFAULT_LOCAL_WORK_SIZE
unsigned const CUDAMiner::c_defaultNumStreams = 2;

void CUDAMiner::configureGPU(unsigned _blockSize, unsigned _gridSize, unsigned _numStreams,
    unsigned _scheduleFlag, unsigned _dagLoadMode, unsigned _parallelHash)
{
    s_dagLoadMode = _dagLoadMode;
    s_blockSize = _blockSize;
    s_gridSize = _gridSize;
    s_numStreams = _numStreams;
    s_scheduleFlag = _scheduleFlag;
    s_parallelHash = _parallelHash;
}

#include <iostream>
#include <fstream>

void CUDAMiner::compileKernel(
    uint64_t period_seed,
    uint64_t dag_elms)
{
    const char* name = "progpow_search";

    std::string text = ProgPow::getKern(period_seed, ProgPow::KERNEL_CUDA);
    text += std::string(CUDAMiner_kernel, sizeof(CUDAMiner_kernel));

    ofstream write;
    write.open("kernel.cu");
    write << text;
    write.close();

    nvrtcProgram prog;
    NVRTC_SAFE_CALL(
        nvrtcCreateProgram(
            &prog,         // prog
            text.c_str(),  // buffer
            "kernel.cu",    // name
            0,             // numHeaders
            NULL,          // headers
            NULL));        // includeNames

    NVRTC_SAFE_CALL(nvrtcAddNameExpression(prog, name));
    cudaDeviceProp device_props;
    CUDA_SAFE_CALL(cudaGetDeviceProperties(&device_props, m_deviceDescriptor.cuDeviceIndex));
    std::string op_arch = "--gpu-architecture=compute_" + to_string(device_props.major) + to_string(device_props.minor);
    std::string op_dag = "-DPROGPOW_DAG_ELEMENTS=" + to_string(dag_elms);

    const char *opts[] = {
        op_arch.c_str(),
        op_dag.c_str(),
        "-lineinfo"
    };
    nvrtcResult compileResult = nvrtcCompileProgram(
        prog,  // prog
        3,     // numOptions
        opts); // options
    // Obtain compilation log from the program.
    size_t logSize;
    NVRTC_SAFE_CALL(nvrtcGetProgramLogSize(prog, &logSize));
    char *log = new char[logSize];
    NVRTC_SAFE_CALL(nvrtcGetProgramLog(prog, log));
    cudalog << "Compile log: " << log;
    delete[] log;
    NVRTC_SAFE_CALL(compileResult);
    // Obtain PTX from the program.
    size_t ptxSize;
    NVRTC_SAFE_CALL(nvrtcGetPTXSize(prog, &ptxSize));
    char *ptx = new char[ptxSize];
    NVRTC_SAFE_CALL(nvrtcGetPTX(prog, ptx));
    write.open("kernel.ptx");
    write << ptx;
    write.close();
    // Load the generated PTX and get a handle to the kernel.
    char *jitInfo = new char[32 * 1024];
    char *jitErr = new char[32 * 1024];
    CUjit_option jitOpt[] = {
        CU_JIT_INFO_LOG_BUFFER,
        CU_JIT_ERROR_LOG_BUFFER,
        CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES,
        CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES,
        CU_JIT_LOG_VERBOSE,
        CU_JIT_GENERATE_LINE_INFO
    };
    void *jitOptVal[] = {
        jitInfo,
        jitErr,
        (void*)(32 * 1024),
        (void*)(32 * 1024),
        (void*)(1),
        (void*)(1)
    };
    CU_SAFE_CALL(cuModuleLoadDataEx(&m_module, ptx, 6, jitOpt, jitOptVal));
    cudalog << "JIT info: \n" << jitInfo;
    cudalog << "JIT err: \n" << jitErr;
    delete[] ptx;
    delete[] jitInfo;
    delete[] jitErr;
    // Find the mangled name
    const char* mangledName;
    NVRTC_SAFE_CALL(nvrtcGetLoweredName(prog, name, &mangledName));
    cudalog << "Mangled name: " << mangledName;
    CU_SAFE_CALL(cuModuleGetFunction(&m_kernel, m_module, mangledName));
    cudalog << "done compiling";
    // Destroy the program.
    NVRTC_SAFE_CALL(nvrtcDestroyProgram(&prog));
}

void CUDAMiner::search(
    uint8_t const* header, uint64_t target, uint64_t start_nonce, const dev::eth::WorkPackage& w)
{
    set_header(*reinterpret_cast<hash32_t const*>(header));
    if (m_current_target != target)
    {
        set_target(target);
        m_current_target = target;
    }

    hash32_t current_header = *reinterpret_cast<hash32_t const *>(header);
    hash64_t* dag;
    get_constants(&dag, NULL, NULL, NULL);

    // prime each stream, clear search result buffers and start the search
    uint32_t current_index;
    for (current_index = 0; current_index < s_numStreams;
         current_index++, start_nonce += m_batch_size)
    {
        cudaStream_t stream = m_streams[current_index];
        volatile Search_results& buffer(*m_search_buf[current_index]);
        buffer.count = 0;

        // Run the batch for this stream
        volatile Search_results *Buffer = &buffer;
        bool hack_false = false;
        void *args[] = {&start_nonce, &current_header, &m_current_target, &dag, &Buffer, &hack_false};
        CU_SAFE_CALL(cuLaunchKernel(m_kernel,
            s_gridSize, 1, 1,   // grid dim
            s_blockSize, 1, 1,  // block dim
            0,                  // shared mem
            stream,             // stream
            args, 0));          // arguments
    }

    // process stream batches until we get new work.
    bool done = false;


    while (!done)
    {
        // Exit next time around if there's new work awaiting
        bool t = true;
        done = m_new_work.compare_exchange_strong(t, false);

        // Check on every batch if we need to suspend mining
        if (!done)
            done = paused();

        // This inner loop will process each cuda stream individually
        for (current_index = 0; current_index < s_numStreams;
             current_index++, start_nonce += m_batch_size)
        {
            // Each pass of this loop will wait for a stream to exit,
            // save any found solutions, then restart the stream
            // on the next group of nonces.
            cudaStream_t stream = m_streams[current_index];

            // Wait for the stream complete
            CUDA_SAFE_CALL(cudaStreamSynchronize(stream));

            if (shouldStop())
            {
                m_new_work.store(false, std::memory_order_relaxed);
                done = true;
            }

            // Detect solutions in current stream's solution buffer
            volatile Search_results& buffer(*m_search_buf[current_index]);
            uint32_t found_count = std::min((unsigned)buffer.count, MAX_SEARCH_RESULTS);

            if (found_count)
            {
                buffer.count = 0;
                uint64_t nonce_base = start_nonce - m_streams_batch_size;

                // Extract solution and pass to higer level
                // using io_service as dispatcher

                for (uint32_t i = 0; i < found_count; i++)
                {
                    h256 mix;
                    uint64_t nonce = nonce_base + buffer.result[i].gid;
                    memcpy(mix.data(), (void*)&buffer.result[i].mix, sizeof(buffer.result[i].mix));
                    auto sol = Solution{nonce, mix, w, std::chrono::steady_clock::now(), m_index};

                    cudalog << EthWhite << "Job: " << w.header.abridged() << " Sol: "
                            << toHex(sol.nonce, HexPrefix::Add) << EthReset;

                    Farm::f().submitProof(sol);
                }
            }

            // restart the stream on the next batch of nonces
            // unless we are done for this round.
            if (!done)
            {
                volatile Search_results *Buffer = &buffer;
                bool hack_false = false;
                void *args[] = {&start_nonce, &current_header, &m_current_target, &dag, &Buffer, &hack_false};
                CU_SAFE_CALL(cuLaunchKernel(m_kernel,
                    s_gridSize, 1, 1,   // grid dim
                    s_blockSize, 1, 1,  // block dim
                    0,                  // shared mem
                    stream,             // stream
                    args, 0));          // arguments
            }
        }

        // Update the hash rate
        updateHashRate(m_batch_size, s_numStreams);

        // Bail out if it's shutdown time
        if (shouldStop())
        {
            m_new_work.store(false, std::memory_order_relaxed);
            break;
        }
    }

#ifdef DEV_BUILD
    // Optionally log job switch time
    if (!shouldStop() && (g_logOptions & LOG_SWITCH))
        cudalog << "Switch time: "
                << std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - m_workSwitchStart)
                       .count()
                << " ms.";
#endif
}
