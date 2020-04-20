//*****************************************************************************
// Copyright 2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <sys/stat.h>

#include <iostream>

#include "config.h"
#include "modelmanager.h"

namespace ovms {

const uint ModelManager::watcherIntervalSec = 1;

Status ModelManager::start() {
    auto& config = ovms::Config::instance();

    // start manager using config file
    if (config.configPath() != "")
        return start(config.configPath());

    // start manager using commandline parameters
    std::shared_ptr<Model> model = std::make_shared<Model>();
    ModelConfig modelConfig;
    modelConfig.name = config.modelName();
    models[modelConfig.name] = std::move(model);
    modelConfig.shape = config.shape();
    modelConfig.basePath = config.modelPath();
    modelConfig.backend = config.targetDevice();
    modelConfig.batchSize = config.batchSize();

    auto status = models[modelConfig.name]->addVersion(modelConfig);
    if (status != Status::OK) {
        // Logger(Log::Warning, "There was an error loading a model ", config.modelName());
        return status;
    }

    return Status::OK;
}

Status ModelManager::start(const std::string& jsonFilename) {
    Status s = loadConfig(jsonFilename);
    if (s != Status::OK) {
        return s;
    }

    std::future<void> exitSignal = exit.get_future();
    std::thread t(std::thread(&ModelManager::watcher, this, std::move(exitSignal)));
    monitor = std::move(t);
    monitor.detach();
    
    return Status::OK;
}

Status ModelManager::loadConfig(const std::string& jsonFilename) {
    rapidjson::Document doc;
    std::ifstream ifs(jsonFilename.c_str());
    // Perform some basic checks on the config file
    if (!ifs.good()) {
        // Logger(Log::Error, "File is invalid ", jsonFilename);
        return Status::FILE_INVALID;
    }

    rapidjson::IStreamWrapper isw(ifs);
    if (doc.ParseStream(isw).HasParseError()) {
        // Logger(Log::Error, "Configuration file is not a valid JSON file.");
        return Status::JSON_INVALID;
    }

    // TODO validate json against schema
    const auto itr = doc.FindMember("model_config_list");
    if (itr == doc.MemberEnd() || !itr->value.IsArray()) {
        // Logger(Log::Error, "Configuration file doesn't have models property.");
        return Status::JSON_INVALID;
    }
    
    models.clear();
    configFilename = jsonFilename;
    for (const auto& configs : itr->value.GetArray()) {
        const auto& v = configs["config"].GetObject();
        std::shared_ptr<Model> model = std::make_shared<Model>();
        ModelConfig modelConfig;
        modelConfig.name = v["name"].GetString();
        modelConfig.basePath = v["base_path"].GetString();
        if (models.find(modelConfig.name) == models.end()) {
            models[modelConfig.name] = std::move(model);
        }

        // Check for optional parameters
        if (v.HasMember("batch_size"))
            modelConfig.batchSize = v["batch_size"].GetUint64();
        if (v.HasMember("target_device"))
            modelConfig.backend = v["target_device"].GetString();
        if (v.HasMember("version"))
            modelConfig.version = v["version"].GetInt64();

        if (v.HasMember("shape")) {
            if (v["shape"].IsString()) {
                modelConfig.addShapes(v["shape"].GetString());
            } else {
                for (auto& s : v["shape"].GetObject()) {
                    std::vector<size_t> shape;
                    for (auto& sh : s.value.GetArray()) {
                        shape.push_back(sh.GetUint64());
                    }
                    modelConfig.shapes[s.name.GetString()] = shape;
                }
            }
        }

        if (v.HasMember("layout")) {
            if (v["layout"].IsString()) {
                modelConfig.addLayouts(v["layout"].GetString());
            } else {
                for (auto& s : v["layout"].GetObject()) {
                    modelConfig.layouts[s.name.GetString()] = s.value.GetString();
                }
            }
        }

        auto status = models[modelConfig.name]->addVersion(modelConfig);
        if (status != Status::OK) {
            // Logger(Log::Warning, "There was an error loading a model ", v["name"].GetString());
            return status;
        }
    }

    return Status::OK;
}

void ModelManager::watcher(std::future<void> exit) {
    // Logger(Log::Info, "Started config watcher thread");
    int64_t lastTime;
    struct stat statTime;

    stat(configFilename.c_str(), &statTime);
    lastTime = statTime.st_ctime;
	while (exit.wait_for(std::chrono::milliseconds(1)) == std::future_status::timeout)
	{
        std::this_thread::sleep_for(std::chrono::seconds(watcherIntervalSec));
        stat(configFilename.c_str(), &statTime);
        if (lastTime != statTime.st_ctime) {
            lastTime = statTime.st_ctime;
            loadConfig(configFilename);
            // Logger(Log::Info, "Configuration changed");
        }
	}
    // Logger(Log::Info, "Exited config watcher thread");
}

void ModelManager::join() {
    exit.set_value();
    monitor.join();
}
  
} // namespace ovms