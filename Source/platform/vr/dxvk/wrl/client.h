/**
 * @file wrl/client.h
 *
 * Microsoft::WRL::ComPtr for builds without the Windows SDK: the part of it the renderer uses.
 */
#pragma once

#include <cstddef>
#include <utility>

#include <unknwn.h>

namespace Microsoft {
namespace WRL {

template <typename T>
class ComPtr {
public:
	ComPtr() = default;
	ComPtr(std::nullptr_t) { }
	ComPtr(T *other)
	    : ptr_(other)
	{
		AddRef();
	}
	ComPtr(const ComPtr &other)
	    : ptr_(other.ptr_)
	{
		AddRef();
	}
	ComPtr(ComPtr &&other) noexcept
	    : ptr_(std::exchange(other.ptr_, nullptr))
	{
	}
	~ComPtr() { Reset(); }

	ComPtr &operator=(std::nullptr_t)
	{
		Reset();
		return *this;
	}
	ComPtr &operator=(T *other)
	{
		ComPtr(other).Swap(*this);
		return *this;
	}
	ComPtr &operator=(const ComPtr &other)
	{
		ComPtr(other).Swap(*this);
		return *this;
	}
	ComPtr &operator=(ComPtr &&other) noexcept
	{
		ComPtr(std::move(other)).Swap(*this);
		return *this;
	}

	T *Get() const { return ptr_; }
	T *operator->() const { return ptr_; }
	explicit operator bool() const { return ptr_ != nullptr; }

	T *const *GetAddressOf() const { return &ptr_; }
	T **GetAddressOf() { return &ptr_; }
	T **ReleaseAndGetAddressOf()
	{
		Reset();
		return &ptr_;
	}
	/** As in WRL, taking the address releases what is held: `&ptr` is an out parameter. */
	T **operator&() { return ReleaseAndGetAddressOf(); }

	unsigned long Reset()
	{
		T *old = std::exchange(ptr_, nullptr);
		return old != nullptr ? old->Release() : 0;
	}
	void Attach(T *other)
	{
		Reset();
		ptr_ = other;
	}
	T *Detach() { return std::exchange(ptr_, nullptr); }
	void Swap(ComPtr &other) noexcept { std::swap(ptr_, other.ptr_); }

	template <typename U>
	HRESULT As(ComPtr<U> *other) const
	{
		return ptr_->QueryInterface(__uuidof(U), reinterpret_cast<void **>(other->ReleaseAndGetAddressOf()));
	}
	HRESULT CopyTo(T **other) const
	{
		AddRef();
		*other = ptr_;
		return S_OK;
	}

private:
	void AddRef() const
	{
		if (ptr_ != nullptr)
			ptr_->AddRef();
	}

	T *ptr_ = nullptr;
};

template <typename T, typename U>
bool operator==(const ComPtr<T> &a, const ComPtr<U> &b) { return a.Get() == b.Get(); }
template <typename T, typename U>
bool operator!=(const ComPtr<T> &a, const ComPtr<U> &b) { return a.Get() != b.Get(); }
template <typename T>
bool operator==(const ComPtr<T> &a, std::nullptr_t) { return a.Get() == nullptr; }
template <typename T>
bool operator!=(const ComPtr<T> &a, std::nullptr_t) { return a.Get() != nullptr; }
template <typename T>
bool operator==(std::nullptr_t, const ComPtr<T> &a) { return a.Get() == nullptr; }
template <typename T>
bool operator!=(std::nullptr_t, const ComPtr<T> &a) { return a.Get() != nullptr; }

} // namespace WRL
} // namespace Microsoft
