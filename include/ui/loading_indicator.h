#pragma once

namespace UiLoadingIndicator {

using Handler = void (*)(bool visible);

void setHandler(Handler handler);
void show();
void hide();
void service();

class Scope {
public:
    Scope() { show(); }
    ~Scope() { hide(); }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
};

}