#pragma once

#include "ui/UiModel.h"

#include <string>

namespace lengjing::config {

class LocalConfig {
public:
    explicit LocalConfig(std::string path);

    const std::string& Path() const noexcept;
    bool Load(ui::UiModel& model, std::string* error = nullptr) const;
    bool Save(const ui::UiModel& model, std::string* error = nullptr) const;

private:
    std::string path_;
};

}  // namespace lengjing::config
