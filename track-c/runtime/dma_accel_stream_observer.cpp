// dma_accel_stream_observer.cpp — see dma_accel_stream_observer.hpp for
// the design rationale.

#include "dma_accel_stream_observer.hpp"

namespace dma_accel {

WaitResult StreamObserver::query(Fence &fence) {
	stream_->pump();
	return fence.is_ready() ? WaitResult::Completed : WaitResult::Pending;
}

WaitResult StreamObserver::wait(Fence &fence, std::chrono::milliseconds timeout) {
	// Passed straight through to Stream::wait(Fence&, int) — see the
	// header comment on Stream::wait() in dma_accel_runtime.hpp for the
	// actual poll/pump loop. Note: a negative `timeout` here would be
	// interpreted by Stream::wait() as "no deadline" (its own
	// convention), same as calling the no-argument wait() overload below
	// — this overload doesn't add a separate check for that, since
	// doing so would just be reintroducing a second way to say the same
	// thing. Prefer the explicit no-argument overload when an
	// indefinite wait is actually what's wanted.
	const bool done = stream_->wait(fence, static_cast<int>(timeout.count()));
	return done ? WaitResult::Completed : WaitResult::Pending;
}

WaitResult StreamObserver::wait(Fence &fence) {
	const bool done = stream_->wait(fence, -1);
	return done ? WaitResult::Completed : WaitResult::Pending;
}

} // namespace dma_accel
