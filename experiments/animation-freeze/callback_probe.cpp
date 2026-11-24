#include "../../reshade-addon/src/freeze.cpp"
#include <cassert>

using namespace freeze;
static const Timing *childTiming;
static LayerType adType{{}, nullptr, nameHash("inkAdvertisementsLayer")};
static LayerType derived{{}, &adType, 0};
static LayerType *type(void *) { return &derived; }
static void consume(Job *job, void *) {
    collectCompletedGroups();
    childTiming = job->timing;
}
static void *copyFixture(void *d, void *s) { return std::memcpy(d,s,0x1b8); }

int main() {
    originalCopy = copyFixture;
    originalActive = consume;
    originalPassive = consume;
    std::array<float,110> src{}, dst{};
    for (int i=0;i<3;++i) src[107+i]=float(i+1);
    set(true);
    assert(copy(dst.data(),src.data()) == dst.data() && ready());
    for (float &f:src) f+=10;
    copy(dst.data(),src.data());
    for (int i=0;i<3;++i) assert(dst[107+i]==float(i+1));
    auto getter = &type;
    auto vtable = &getter;
    Timing input{};
    input.fill(3);
    Job job{&vtable,&input};
    Completion handle{{},1,1};
    auto *handlePtr = &handle;
    JobGroup group{{}, &handlePtr};
    active(&job,&group);
    assert(job.timing==&input && childTiming!=&input && matches()==1);
    auto savedTiming=childTiming;
    passive(&job,&group);
    assert(matches()==2 && childTiming==savedTiming);
    for (size_t i=0;i<24;++i) assert((*savedTiming)[i]==(i<4?0:3));
    set(false);
    copy(dst.data(),src.data());
    assert(!ready() && dst[107]==1);
    for (float &f:src) ++f;
    copy(dst.data(),src.data());
    assert(dst[107]==2);
    active(&job,&group);
    assert(childTiming==&input);
    assert(handle.references == 2 && (*savedTiming)[1] == 0);
    InterlockedDecrement(&handle.outstanding);
    collectCompletedGroups();
    assert(handle.references == 1 && freeze::retained.empty());
    set(true);
    for (unsigned i = 0; i < 70000; ++i) {
        std::memcpy(input.data() + 4, &i, sizeof(i));
        active(&job, &group);
        assert(childTiming != &input);
        collectCompletedGroups();
        assert(freeze::retained.empty());
    }
    assert(!install()); // The fixture executable is an unsupported image.
}
