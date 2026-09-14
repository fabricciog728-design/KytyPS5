#ifndef KYTY_COMMON_UNIQUEFUNCTION_H_
#define KYTY_COMMON_UNIQUEFUNCTION_H_

#include <memory>
#include <type_traits>
#include <utility>

namespace Common {
template <typename Result, typename... Args>
class UniqueFunction {
	class CallableBase {
	public:
		virtual ~CallableBase()               = default;
		virtual Result Invoke(Args&&... args) = 0;
		virtual void   MoveTo(CallableBase* dst) noexcept = 0;
	};

	template <typename Function>
	class Callable final: public CallableBase {
	public:
		explicit Callable(Function function): m_function(std::move(function)) {}

		Result Invoke(Args&&... args) override { return m_function(std::forward<Args>(args)...); }

		void MoveTo(CallableBase* dst) noexcept override {
			new (dst) Callable(std::move(m_function));
		}

	private:
		Function m_function;
	};

	// Small-buffer optimization: callables up to 56 bytes (plus vptr) live
	// inline, so the hot GPU/scheduler queues avoid a heap alloc per command.
	// Anything bigger, over-aligned, or throwing on move falls back to the heap
	// with identical semantics.
	static constexpr size_t kInlineSize  = 64;
	static constexpr size_t kInlineAlign = alignof(void*);

	CallableBase* CallableAtInline() noexcept {
		return reinterpret_cast<CallableBase*>(m_storage);
	}

	void Reset() noexcept {
		if (m_callable == nullptr) {
			return;
		}
		if (m_local) {
			m_callable->~CallableBase();
		} else {
			delete m_callable;
		}
		m_callable = nullptr;
		m_local    = false;
	}

public:
	UniqueFunction() = default;

	~UniqueFunction() { Reset(); }

	template <typename Function>
	UniqueFunction(Function&& function) {
		using Model = Callable<std::decay_t<Function>>;
		if constexpr (sizeof(Model) <= kInlineSize && alignof(Model) <= kInlineAlign &&
		              std::is_nothrow_move_constructible_v<std::decay_t<Function>>) {
			m_callable = new (m_storage) Model(std::forward<Function>(function));
			m_local    = true;
		} else {
			m_callable = new Model(std::forward<Function>(function));
			m_local    = false;
		}
	}

	UniqueFunction(UniqueFunction&& other) noexcept: m_callable(nullptr), m_local(false) {
		if (other.m_local && other.m_callable != nullptr) {
			other.m_callable->MoveTo(CallableAtInline());
			other.m_callable->~CallableBase();
			other.m_callable = nullptr;
			other.m_local    = false;
			m_callable       = CallableAtInline();
			m_local          = true;
		} else {
			m_callable       = other.m_callable;
			m_local          = false;
			other.m_callable = nullptr;
		}
	}

	UniqueFunction& operator=(UniqueFunction&& other) noexcept {
		if (this == &other) {
			return *this;
		}
		Reset();
		if (other.m_local && other.m_callable != nullptr) {
			other.m_callable->MoveTo(CallableAtInline());
			other.m_callable->~CallableBase();
			other.m_callable = nullptr;
			other.m_local    = false;
			m_callable       = CallableAtInline();
			m_local          = true;
		} else {
			m_callable       = other.m_callable;
			m_local          = false;
			other.m_callable = nullptr;
		}
		return *this;
	}

	UniqueFunction(const UniqueFunction&)            = delete;
	UniqueFunction& operator=(const UniqueFunction&) = delete;

	Result operator()(Args... args) const {
		return m_callable->Invoke(std::forward<Args>(args)...);
	}

	explicit operator bool() const noexcept { return m_callable != nullptr; }

private:
	alignas(void*) unsigned char m_storage[kInlineSize];
	CallableBase*                m_callable = nullptr;
	bool                         m_local    = false;
};

} // namespace Common

#endif // KYTY_COMMON_UNIQUEFUNCTION_H_
