#include "sampleMNISTAPI.h"

GIEMnistAPI::GIEMnistAPI(void)
{
	mRuntime = NULL;
	mEngine = NULL;
	mContext = NULL;
}

GIEMnistAPI::~GIEMnistAPI()
{
	if(mEngine != NULL){
		std::cout<<"distruggo l'istanza ICudaEngine"<< std::endl;
		mEngine->destroy();
		mEngine = NULL;
	}

	if(mRuntime != NULL){
		std::cout<<"distruggo l'istanza IRuntime"<< std::endl;
		mRuntime->destroy();
		mRuntime = NULL;
	}	
}

void GIEMnistAPI::initAndLunch()
{
	// create a model using the API directly and serialize it to a stream
    IHostMemory *modelStream{nullptr};

    /**
	 *	fase di building
	 */
	APIToModel(1, &modelStream);

	// read a random digit file
	uint8_t* fileData = loadPGMFile();

	// print an ascii representation
	plotPGMasAsci(fileData);
	
	// parse the mean file produced by caffe and subtract it from the image
	float* data = subtractImage(fileData);

	//istanzione l'oggeto per l'inferenza in tempo reale
	mRuntime = createInferRuntime(gLogger);
	//deseriliazzo il modello per fare l'inferenza
	mEngine = mRuntime->deserializeCudaEngine(modelStream->data(), modelStream->size(), nullptr);
    if (modelStream) modelStream->destroy();

	//creao il constro per l'esecuzione dell'inferenza
	mContext = mEngine->createExecutionContext();

    /**
	 *	fase di deploy 
	 */
	doInference(*mContext, data, prob, 1);

	// destroy the engine
	mContext->destroy();
	mEngine->destroy();
	mRuntime->destroy();

	// print a histogram of the output distribution
	if(plotAsciResult()){
		std::cout<<"tutto è andato ok"<<std::endl;
	}
}
// We have the data files located in a specific directory. This 
// searches for that directory format from the current directory.
std::string GIEMnistAPI::locateFile(const std::string& input)
{
	std::string file = "data/samples/mnist/" + input;
	struct stat info;
	int i, MAX_DEPTH = 10;
	for (i = 0; i < MAX_DEPTH && stat(file.c_str(), &info); i++)
		file = "../" + file;

    if (i == MAX_DEPTH)
    {
		file = std::string("data/mnist/") + input;
		for (i = 0; i < MAX_DEPTH && stat(file.c_str(), &info); i++)
			file = "../" + file;		
    }

	assert(i != MAX_DEPTH);

	return file;
}

// simple PGM (portable greyscale map) reader
void GIEMnistAPI::readPGMFile(const std::string& fileName,  uint8_t buffer[INPUT_H*INPUT_W])
{
	std::ifstream infile(locateFile(fileName), std::ifstream::binary);
	std::string magic, h, w, max;
	infile >> magic >> h >> w >> max;
	infile.seekg(1, infile.cur);
	infile.read(reinterpret_cast<char*>(buffer), INPUT_H*INPUT_W);
}

uint8_t* GIEMnistAPI::loadPGMFile()
{
	// read a random digit file
	srand(unsigned(time(nullptr)));
	uint8_t * fileData = new uint8_t [INPUT_H*INPUT_W];
    int num = rand() % 10;
	readPGMFile(std::to_string(num) + ".pgm", fileData);
	std::cout <<"indirizzo di fileData: " << &fileData << std::endl;
	return fileData;
}

void GIEMnistAPI::plotPGMasAsci(uint8_t fileData[])
{
	// print an ascii representation
	std::cout << "\n\n\n---------------------------" << "\n\n\n" << std::endl;
	for (int i = 0; i < INPUT_H*INPUT_W; i++)
		std::cout << (" .:-=+*#%@"[fileData[i] / 26]) << (((i + 1) % INPUT_W) ? "" : "\n");
}

float* GIEMnistAPI::subtractImage(uint8_t* currentImg)
{
	// parse the mean file produced by caffe and subtract it from the image
	ICaffeParser* parser = createCaffeParser();
	IBinaryProtoBlob* meanBlob = parser->parseBinaryProto(locateFile("mnist_mean.binaryproto").c_str());
	parser->destroy();
	const float *meanData = reinterpret_cast<const float*>(meanBlob->getData());

	float* data = new float[INPUT_H*INPUT_W];
	for (int i = 0; i < INPUT_H*INPUT_W; i++)
		data[i] = float(currentImg[i])-meanData[i];

	meanBlob->destroy();
	return data;
}

// Our weight files are in a very simple space delimited format.
// [type] [size] <data x size in hex> 
std::map<std::string, Weights> GIEMnistAPI::loadWeights(const std::string file)
{
    std::map<std::string, Weights> weightMap;
	std::ifstream input(file);
	assert(input.is_open() && "Unable to load weight file.");
    int32_t count;
    input >> count;
    assert(count > 0 && "Invalid weight map file.");
    while(count--)
    {
        Weights wt{DataType::kFLOAT, nullptr, 0};
        uint32_t type, size;
        std::string name;
        input >> name >> std::dec >> type >> size;
        wt.type = static_cast<DataType>(type);
        if (wt.type == DataType::kFLOAT)
        {
            uint32_t *val = reinterpret_cast<uint32_t*>(malloc(sizeof(val) * size));
            for (uint32_t x = 0, y = size; x < y; ++x)
            {
                input >> std::hex >> val[x];

            }
            wt.values = val;
        } else if (wt.type == DataType::kHALF)
        {
            uint16_t *val = reinterpret_cast<uint16_t*>(malloc(sizeof(val) * size));
            for (uint32_t x = 0, y = size; x < y; ++x)
            {
                input >> std::hex >> val[x];
            }
            wt.values = val;
        }
        wt.count = size;
        weightMap[name] = wt;
    }
    return weightMap;
}



// Creat the Engine using only the API and not any parser.
ICudaEngine *
GIEMnistAPI::createMNISTEngine(unsigned int maxBatchSize, IBuilder *builder, DataType dt)
{
	INetworkDefinition* network = builder->createNetwork();

	//  Create input of shape { 1, 1, 28, 28 } with name referenced by INPUT_BLOB_NAME
	auto data = network->addInput(INPUT_BLOB_NAME, dt, DimsCHW{ 1, INPUT_H, INPUT_W});
	assert(data != nullptr);

	// Create a scale layer with default power/shift and specified scale parameter.
	float scale_param = 0.0125f;
	Weights power{DataType::kFLOAT, nullptr, 0};
	Weights shift{DataType::kFLOAT, nullptr, 0};
	Weights scale{DataType::kFLOAT, &scale_param, 1};
	auto scale_1 = network->addScale(*data,	ScaleMode::kUNIFORM, shift, scale, power);
	assert(scale_1 != nullptr);

	// Add a convolution layer with 20 outputs and a 5x5 filter.
    std::map<std::string, Weights> weightMap = loadWeights(locateFile("mnistapi.wts"));
	auto conv1 = network->addConvolution(*scale_1->getOutput(0), 20, DimsHW{5, 5}, weightMap["conv1filter"], weightMap["conv1bias"]);
	assert(conv1 != nullptr);
	conv1->setStride(DimsHW{1, 1});

	// Add a max pooling layer with stride of 2x2 and kernel size of 2x2.
	auto pool1 = network->addPooling(*conv1->getOutput(0), PoolingType::kMAX, DimsHW{2, 2});
	assert(pool1 != nullptr);
	pool1->setStride(DimsHW{2, 2});

	// Add a second convolution layer with 50 outputs and a 5x5 filter.
	auto conv2 = network->addConvolution(*pool1->getOutput(0), 50, DimsHW{5, 5}, weightMap["conv2filter"], weightMap["conv2bias"]);
	assert(conv2 != nullptr);
	conv2->setStride(DimsHW{1, 1});

	// Add a second max pooling layer with stride of 2x2 and kernel size of 2x3>
	auto pool2 = network->addPooling(*conv2->getOutput(0), PoolingType::kMAX, DimsHW{2, 2});
	assert(pool2 != nullptr);
	pool2->setStride(DimsHW{2, 2});

	// Add a fully connected layer with 500 outputs.
	auto ip1 = network->addFullyConnected(*pool2->getOutput(0), 500, weightMap["ip1filter"], weightMap["ip1bias"]);
	assert(ip1 != nullptr);

	// Add an activation layer using the ReLU algorithm.
	auto relu1 = network->addActivation(*ip1->getOutput(0), ActivationType::kRELU);
	assert(relu1 != nullptr);

	// Add a second fully connected layer with 20 outputs.
	auto ip2 = network->addFullyConnected(*relu1->getOutput(0), OUTPUT_SIZE, weightMap["ip2filter"], weightMap["ip2bias"]);
	assert(ip2 != nullptr);

	// Add a softmax layer to determine the probability.
	auto prob = network->addSoftMax(*ip2->getOutput(0));
	assert(prob != nullptr);
	prob->getOutput(0)->setName(OUTPUT_BLOB_NAME);
	network->markOutput(*prob->getOutput(0));

	// Build the engine
	builder->setMaxBatchSize(maxBatchSize);
	builder->setMaxWorkspaceSize(1 << 20);

	auto engine = builder->buildCudaEngine(*network);
	// we don't need the network any more
	network->destroy();

	// Once we have built the cuda engine, we can release all of our held memory.
	for (auto &mem : weightMap)
    {
        free((void*)(mem.second.values));
    }
	return engine;
}

void GIEMnistAPI::APIToModel(unsigned int maxBatchSize, // batch size - NB must be at least as large as the batch we want to run with)
		     IHostMemory **modelStream)
{
	// create the builder
	IBuilder* builder = createInferBuilder(gLogger);

	// create the model to populate the network, then set the outputs and create an engine
	ICudaEngine* engine = createMNISTEngine(maxBatchSize, builder, DataType::kFLOAT);

	assert(engine != nullptr);

	// serialize the engine, then close everything down
	(*modelStream) = engine->serialize();
	engine->destroy();
	builder->destroy();
}

void GIEMnistAPI::doInference(IExecutionContext& context, float* input, float* output, int batchSize)
{
	const ICudaEngine& engine = context.getEngine();
	// input and output buffer pointers that we pass to the engine - the engine requires exactly IEngine::getNbBindings(),
	// of these, but in this case we know that there is exactly one input and one output.
	assert(engine.getNbBindings() == 2);
	void* buffers[2];

	// In order to bind the buffers, we need to know the names of the input and output tensors.
	// note that indices are guaranteed to be less than IEngine::getNbBindings()
	int inputIndex = engine.getBindingIndex(INPUT_BLOB_NAME), 
	    outputIndex = engine.getBindingIndex(OUTPUT_BLOB_NAME);

	// create GPU buffers and a stream
	CHECK(cudaMalloc(&buffers[inputIndex], batchSize * INPUT_H * INPUT_W * sizeof(float)));
	CHECK(cudaMalloc(&buffers[outputIndex], batchSize * OUTPUT_SIZE * sizeof(float)));

	cudaStream_t stream;
	CHECK(cudaStreamCreate(&stream));

	// DMA the input to the GPU,  execute the batch asynchronously, and DMA it back:
	CHECK(cudaMemcpyAsync(buffers[inputIndex], input, batchSize * INPUT_H * INPUT_W * sizeof(float), cudaMemcpyHostToDevice, stream));
	context.enqueue(batchSize, buffers, stream, nullptr);
	CHECK(cudaMemcpyAsync(output, buffers[outputIndex], batchSize * OUTPUT_SIZE*sizeof(float), cudaMemcpyDeviceToHost, stream));
	cudaStreamSynchronize(stream);

	// release the stream and the buffers
	cudaStreamDestroy(stream);
	CHECK(cudaFree(buffers[inputIndex]));
	CHECK(cudaFree(buffers[outputIndex]));
}

bool GIEMnistAPI::plotAsciResult()
{
	// print a histogram of the output distribution
	std::cout << "\n\n";
    float val{0.0f};
    int idx{0};
	for (unsigned int i = 0; i < 10; i++)
    {
        val = std::max(val, prob[i]);
        if (val == prob[i]) idx = i;
		std::cout << i << ": " << std::string(int(std::floor(prob[i] * 10 + 0.5f)), '*') << "\n";
    }
	std::cout << std::endl;

	return (idx == getNum() && val > 0.9f) ? EXIT_SUCCESS : EXIT_FAILURE;
}
