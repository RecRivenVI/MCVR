#pragma once

class DlssEvaluateState {
  public:
    enum class Output {
        None,
        Dlss,
        Fallback,
    };

    void begin() { output_ = Output::None; }

    void complete(bool success) {
        output_ = success ? Output::Dlss : Output::None;
        historyResetPending_ = !success;
    }

    void completeFallback() {
        output_ = Output::Fallback;
        historyResetPending_ = true;
    }

    [[nodiscard]] bool outputValid() const { return output_ != Output::None; }
    [[nodiscard]] bool dlssOutputValid() const { return output_ == Output::Dlss; }
    [[nodiscard]] bool fallbackOutputValid() const { return output_ == Output::Fallback; }
    [[nodiscard]] bool historyResetPending() const { return historyResetPending_; }

  private:
    Output output_ = Output::None;
    bool historyResetPending_ = true;
};
