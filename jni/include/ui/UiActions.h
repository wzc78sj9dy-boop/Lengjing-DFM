#pragma once

#include "ui/UiModel.h"

namespace lengjing::ui {

class UiActions {
public:
    virtual ~UiActions() = default;

    virtual void StartRuntime() = 0;
    virtual void StopRuntime() = 0;
    virtual void HideMenu() = 0;
    virtual void ExitApplication() = 0;
    virtual void ClearLogs() = 0;
    virtual void ResetLocalSettings() = 0;
    virtual void ReloadCustomItems() = 0;
    virtual void AimEnabledChanged(bool enabled) = 0;
    virtual void SettingsChanged(SettingsDomain domain) = 0;
};

}  // namespace lengjing::ui
