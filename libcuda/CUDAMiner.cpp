/*  Blah, blah, blah.. all this pedantic nonsense to say that this
    source code is made available under the terms and conditions
    of the accompanying GNU General Public License */

#include "CUDAMiner.h"
#include "libdevcore/Log.h"

using namespace std;
using namespace dev;
using namespace eth;

unsigned CUDAMiner::s_numInstances = 0;

vector<int> CUDAMiner::s_devices(MAX_MINERS, -1);

CUDAMiner::CUDAMiner(FarmFace& _farm, unsigned _index) :
	Miner("cuda-", _farm, _index),
	m_light(getNumDevices()) {}

CUDAMiner::~CUDAMiner()
{
}

bool CUDAMiner::init(const h256& seed)
{
	try {
		if (s_dagLoadMode == DAG_LOAD_MODE_SEQUENTIAL)
			while (s_dagLoadIndex < index)
				this_thread::sleep_for(chrono::milliseconds(100));
		unsigned device = s_devices[index] > -1 ? s_devices[index] : index;

		loginfo(workerName() << " - Initialising miner " << index);

		EthashAux::LightType light;
		light = EthashAux::light(seed);
		bytesConstRef lightData = light->data();

		cuda_init(getNumDevices(), light->light, lightData.data(), lightData.size(),
		          device, (s_dagLoadMode == DAG_LOAD_MODE_SINGLE), s_dagInHostMemory, s_dagCreateDevice);
		s_dagLoadIndex++;

		if (s_dagLoadMode == DAG_LOAD_MODE_SINGLE) {
			if (s_dagLoadIndex >= s_numInstances && s_dagInHostMemory) {
				// all devices have loaded DAG, we can free now
				delete[] s_dagInHostMemory;
				s_dagInHostMemory = NULL;
				loginfo(workerName() << " - Freeing DAG from host");
			}
		}
		return true;
	}
	catch (std::exception const& _e) {
		logerror(workerName() << " - Error CUDA mining: " << _e.what());
		throw;
	}
}

void CUDAMiner::workLoop()
{
	WorkPackage current;
	current.header = h256{1u};
	current.seed = h256{1u};

	try {
		while (true) {
			// take local copy of work since it may end up being overwritten.
			const WorkPackage w = work();

			if (current.header != w.header || current.seed != w.seed) {
				if (!w || w.header == h256()) {
					logwarn(workerName() << " - No work. Pause for 3 s.");
					std::this_thread::sleep_for(std::chrono::seconds(3));
					continue;
				}
				if (current.seed != w.seed)
					if (!init(w.seed))
						break;
				current = w;
			}
			uint64_t upper64OfBoundary = (uint64_t)(u64)((u256)current.boundary >> 192);
			uint64_t startN = current.startNonce;
			if (current.exSizeBits >= 0) {
				// this can support up to 2^MAX_GPU devices
				startN = current.startNonce | ((uint64_t)index << (64 - LOG2_MAX_MINERS - current.exSizeBits));
			}
			search(current.header.data(), upper64OfBoundary, (current.exSizeBits >= 0), startN, w);
		}

		// Reset miner and stop working
		CUDA_SAFE_CALL(cudaDeviceReset());
	}
	catch (std::exception const& _e) {
		logerror(workerName() << " - Fatal GPU error: " << _e.what());
		throw;
	}
}

void CUDAMiner::kick_miner()
{
	m_new_work.store(true, memory_order_relaxed);
}

void CUDAMiner::setNumInstances(unsigned _instances)
{
	s_numInstances = std::min<unsigned>(_instances, getNumDevices());
}

void CUDAMiner::setDevices(const vector<unsigned>& _devices, unsigned _selectedDeviceCount)
{
	for (unsigned i = 0; i < _selectedDeviceCount; i++)
		s_devices[i] = _devices[i];
}

unsigned CUDAMiner::getNumDevices()
{
	int deviceCount = -1;
	cudaError_t err = cudaGetDeviceCount(&deviceCount);
	if (err == cudaSuccess)
		return deviceCount;

	if (err == cudaErrorInsufficientDriver) {
		int driverVersion = -1;
		cudaDriverGetVersion(&driverVersion);
		if (driverVersion == 0)
			throw std::runtime_error{"No CUDA driver found"};
		throw std::runtime_error{"Insufficient CUDA driver: " + std::to_string(driverVersion)};
	}

	throw std::runtime_error{cudaGetErrorString(err)};
}

void CUDAMiner::listDevices()
{
	cout << "\nListing CUDA devices.\nFORMAT: [deviceID] deviceName\n";
	try {
		int numDevices = getNumDevices();
		for (int i = 0; i < numDevices; ++i) {
			cudaDeviceProp props;
			CUDA_SAFE_CALL(cudaGetDeviceProperties(&props, i));

			cout << "[" + to_string(i) + "] " + string(props.name) + "\n";
			cout << "\tCompute version: " + to_string(props.major) + "." + to_string(props.minor) + "\n";
			cout << "\tcudaDeviceProp::totalGlobalMem: " + to_string(props.totalGlobalMem) + "\n";
			cout << "\tPci: " << setw(4) << setfill('0') << hex << props.pciDomainID << ':' << setw(2)
			     << props.pciBusID << ':' << setw(2) << props.pciDeviceID << '\n';
		}
	}
	catch (std::exception const&) {
	}
}

bool CUDAMiner::configureGPU(
    unsigned _blockSize,
    unsigned _gridSize,
    unsigned _numStreams,
    unsigned _scheduleFlag,
    uint64_t _currentBlock,
    unsigned _dagLoadMode,
    unsigned _dagCreateDevice,
    bool _eval
)
{
	s_dagLoadMode = _dagLoadMode;
	s_dagCreateDevice = _dagCreateDevice;

	if (!cuda_configureGPU(
	        getNumDevices(),
	        s_devices,
	        ((_blockSize + 7) / 8) * 8,
	        _gridSize,
	        _numStreams,
	        _scheduleFlag,
	        _currentBlock,
	        _eval)
	   ) {
		cout << "No CUDA device with sufficient memory was found. Can't CUDA mine. Remove the -U argument" << endl;
		return false;
	}
	return true;
}

void CUDAMiner::setParallelHash(unsigned _parallelHash)
{
	s_parallelHash = _parallelHash;
}

bool CUDAMiner::cuda_configureGPU(
    size_t numDevices,
    const vector<int>& _devices,
    unsigned _blockSize,
    unsigned _gridSize,
    unsigned _numStreams,
    unsigned _scheduleFlag,
    uint64_t _currentBlock,
    bool _eval
)
{
	try {
		s_blockSize = _blockSize;
		s_gridSize = _gridSize;
		s_numStreams = _numStreams;
		s_scheduleFlag = _scheduleFlag;
		s_eval = _eval;

		loginfo("Using grid size " << s_gridSize << ", block size " << s_blockSize);

		// by default let's only consider the DAG of the first epoch
		uint64_t dagSize = ethash_get_datasize(_currentBlock);
		int devicesCount = static_cast<int>(numDevices);
		for (int i = 0; i < devicesCount; i++) {
			if (_devices[i] != -1) {
				int deviceId = min(devicesCount - 1, _devices[i]);
				cudaDeviceProp props;
				CUDA_SAFE_CALL(cudaGetDeviceProperties(&props, deviceId));
				if (props.totalGlobalMem >= dagSize) {
					loginfo("Found suitable CUDA device [" << string(props.name) << "] with " <<
					        props.totalGlobalMem / (1024 * 1024) <<
					        " MB of GPU memory");
				}
				else {
					logerror("CUDA device " << string(props.name) << " has insufficient GPU memory." <<
					         props.totalGlobalMem / (1024 * 1024) <<
					         " MB of memory found < " << dagSize << " bytes of memory required");
					return false;
				}
			}
		}
		return true;
	}
	catch (std::exception const& _e) {
		logerror("Error CUDA mining: " << _e.what());
		throw;
	}
}

unsigned CUDAMiner::s_parallelHash;
unsigned CUDAMiner::s_blockSize;
unsigned CUDAMiner::s_gridSize;
unsigned CUDAMiner::s_numStreams;
unsigned CUDAMiner::s_scheduleFlag;
bool CUDAMiner::s_eval = false;

bool CUDAMiner::cuda_init(
    size_t numDevices,
    ethash_light_t _light,
    uint8_t const* _lightData,
    uint64_t _lightSize,
    unsigned _deviceId,
    bool _cpyToHost,
    uint8_t*& hostDAG,
    unsigned dagCreateDevice)
{
	try {
		if (numDevices == 0)
			return false;

		// use selected device
		m_device_num = _deviceId < numDevices - 1 ? _deviceId : numDevices - 1;
		m_hwmoninfo.deviceType = HwMonitorInfoType::NVIDIA;
		m_hwmoninfo.indexSource = HwMonitorIndexSource::CUDA;
		m_hwmoninfo.deviceIndex = m_device_num;

		cudaDeviceProp device_props;
		CUDA_SAFE_CALL(cudaGetDeviceProperties(&device_props, m_device_num));

		loginfo(workerName() << " - Using device: " << device_props.name << " (Compute " + to_string(
		            device_props.major) + "." + to_string(
		            device_props.minor) + ")");
		m_hwmoninfo.deviceName = device_props.name;
		stringstream ss;
		ss << setw(4) << setfill('0') << hex << device_props.pciDomainID << ':' << setw(2)
		   << device_props.pciBusID << ':' << setw(2) << device_props.pciDeviceID;
		m_hwmoninfo.deviceId = ss.str();

		m_search_buf = new volatile search_results *[s_numStreams];
		m_streams = new cudaStream_t[s_numStreams];

		uint64_t dagSize = ethash_get_datasize(_light->block_number);
		uint32_t dagSize128   = (unsigned)(dagSize / ETHASH_MIX_BYTES);
		uint32_t lightSize64 = (unsigned)(_lightSize / sizeof(node));

		CUDA_SAFE_CALL(cudaSetDevice(m_device_num));
		if (dagSize128 != m_dag_size || !m_dag) {
			//Check whether the current device has sufficient memory everytime we recreate the dag
			if (device_props.totalGlobalMem < dagSize) {
				logerror(workerName() << " - CUDA device " << string(device_props.name) << " has insufficient GPU memory." <<
				         device_props.totalGlobalMem << " bytes of memory found < " << dagSize << " bytes of memory required");
				return false;
			}
			//We need to reset the device and recreate the dag
			logwarn(workerName() << " - Resetting device");
			CUDA_SAFE_CALL(cudaDeviceReset());
			CUDA_SAFE_CALL(cudaSetDeviceFlags(s_scheduleFlag));
			CUDA_SAFE_CALL(cudaDeviceSetCacheConfig(cudaFuncCachePreferL1));
			//We need to reset the light and the Dag for the following code to reallocate
			//since cudaDeviceReset() free's all previous allocated memory
			m_light[m_device_num] = nullptr;
			m_dag = nullptr;
			loginfo(workerName() << " - Device successfully reset");
		}
		// create buffer for cache
		hash128_t* dag = m_dag;
		hash64_t* light = m_light[m_device_num];

		if (!light) {
			loginfo(workerName() << " - Allocating light with size: " << _lightSize / 1024 << " KB");
			CUDA_SAFE_CALL(cudaMalloc(reinterpret_cast<void**>(&light), _lightSize));
		}
		// copy lightData to device
		CUDA_SAFE_CALL(cudaMemcpy(reinterpret_cast<void*>(light), _lightData, _lightSize, cudaMemcpyHostToDevice));
		m_light[m_device_num] = light;

		if (dagSize128 != m_dag_size || !dag) // create buffer for dag
			CUDA_SAFE_CALL(cudaMalloc(reinterpret_cast<void**>(&dag), dagSize));

		set_constants(dag, dagSize128, light, lightSize64); //in ethash_cuda_miner_kernel.cu
		auto startDAG = std::chrono::steady_clock::now();

		if (dagSize128 != m_dag_size || !dag) {
			// create mining buffers
			for (unsigned i = 0; i != s_numStreams; ++i) {
				CUDA_SAFE_CALL(cudaMallocHost(&m_search_buf[i], sizeof(search_results)));
				CUDA_SAFE_CALL(cudaStreamCreate(&m_streams[i]));
			}

			if (!hostDAG) {
				if ((m_device_num == dagCreateDevice) || !_cpyToHost) { //if !cpyToHost -> All devices shall generate their DAG
					loginfo(workerName() << " - Generating DAG, size: " << dagSize / (1024 * 1024) << " MB");

					ethash_generate_dag(dagSize, s_gridSize, s_blockSize, m_streams[0]);

					if (_cpyToHost) {
						uint8_t* memoryDAG = new uint8_t[dagSize];
						loginfo(workerName() << " - Copying DAG from GPU" << m_device_num << " to host");
						CUDA_SAFE_CALL(cudaMemcpy(reinterpret_cast<void*>(memoryDAG), dag, dagSize, cudaMemcpyDeviceToHost));

						hostDAG = memoryDAG;
					}
				}
				else {
					while (!hostDAG)
						this_thread::sleep_for(chrono::milliseconds(100));
					goto cpyDag;
				}
			}
			else {
			cpyDag:
				loginfo(workerName() << " - Copying DAG from host to GPU" << m_device_num);
				const void* hdag = (const void*)hostDAG;
				CUDA_SAFE_CALL(cudaMemcpy(reinterpret_cast<void*>(dag), hdag, dagSize, cudaMemcpyHostToDevice));
			}
		}

		auto dagTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startDAG);

		loginfo(workerName() << " - DAG Generated in " << dagTime.count() << " ms.");
		m_dag = dag;
		m_dag_size = dagSize128;
		return true;
	}
	catch (std::exception const& _e) {
		logerror(workerName() << " - Error CUDA mining: " << _e.what());
		throw;
	}
}

void CUDAMiner::search(
    uint8_t const* header,
    uint64_t target,
    bool _ethStratum,
    uint64_t _startN,
    const dev::eth::WorkPackage& w)
{

	set_header_and_target(*reinterpret_cast<hash32_t const*>(header), target);
	uint64_t current_nonce = _ethStratum ? _startN : get_start_nonce();

	const uint32_t batch_size = s_gridSize * s_blockSize;
	uint32_t current_index;
	for (current_index = 0; current_index < s_numStreams; current_index++, current_nonce += batch_size) {
		m_search_buf[current_index]->count = 0;
		run_ethash_search(
		    s_gridSize, s_blockSize, m_streams[current_index], m_search_buf[current_index], current_nonce, s_parallelHash);
	}

	const uint32_t full_batch_size = s_numStreams * batch_size;
	bool done = false;
	while (!done) {

		bool t = true;
		if (m_new_work.compare_exchange_strong(t, false, memory_order_relaxed))
			done = true;

		for (current_index = 0; current_index < s_numStreams; current_index++, current_nonce += batch_size) {

			cudaStream_t stream = m_streams[current_index];
			volatile search_results* buffer = m_search_buf[current_index];

			CUDA_SAFE_CALL(cudaStreamSynchronize(stream));

			search_results r = *((search_results*)buffer);
			if (r.count)
				buffer->count = 0;

			if (!done)
				run_ethash_search(s_gridSize, s_blockSize, stream, buffer, current_nonce, s_parallelHash);

			if (r.count) {
				uint64_t nonce = (current_nonce - full_batch_size) + r.gid;
				if (s_eval) {
					Result r = EthashAux::eval(w.seed, w.header, nonce);
					if (r.value < w.boundary)
						farm.submitProof(Solution{workerName().c_str(), nonce, r.mixHash, w, m_new_work});
					else {
						farm.failedSolution();
						logwarn(workerName() << " - Incorrect result discarded!");
					}
				}
				else {
					h256 mix;
					memcpy(mix.data(), r.mix, sizeof(r.mix));
					farm.submitProof(Solution{workerName().c_str(), nonce, mix, w, m_new_work});
				}
			}

			addHashCount(batch_size);
		}
	}
	if (g_logSwitchTime) {
		loginfo(workerName() << " - switch time " << std::chrono::duration_cast<std::chrono::milliseconds>
		        (std::chrono::high_resolution_clock::now() - workSwitchStart).count() << " ms.");
	}
}

