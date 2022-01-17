#include <algorithm>
#include <cassert>
#include <mutex>
#include <scip/type_result.h>
#include <scip/type_retcode.h>
#include <type_traits>
#include <utility>

#include <objscip/objbranchrule.h>
#include <objscip/objheur.h>
#include <scip/scip.h>
#include <scip/scipdefplugins.h>

#include "ecole/scip/scimpl.hpp"
#include "ecole/scip/utils.hpp"
#include "ecole/utility/coroutine.hpp"

namespace ecole::scip {

/******************************************
 *  Declaration of the ReverseBranchrule  *
 ******************************************/

namespace {

class ReverseBranchrule : public ::scip::ObjBranchrule {
public:
	static constexpr int max_priority = 536870911;
	static constexpr int no_maxdepth = -1;
	static constexpr double no_maxbounddist = 1.0;
	static constexpr auto name = "ecole::ReverseBranchrule";

	ReverseBranchrule(SCIP* scip, std::weak_ptr<utility::Controller::Executor> /*weak_executor_*/);

	auto scip_execlp(SCIP* scip, SCIP_BRANCHRULE* branchrule, SCIP_Bool allowaddcons, SCIP_RESULT* result)
		-> SCIP_RETCODE override;

private:
	std::weak_ptr<utility::Controller::Executor> weak_executor;
};

}  // namespace

/************************************
 *  Declaration of the ReverseHeur  *
 ************************************/

namespace {

class ReverseHeur : public ::scip::ObjHeur {
public:
	static constexpr int max_priority = 536870911;
	static constexpr auto name = "ecole::ReverseHeur";

	ReverseHeur(
		SCIP* scip,
		std::weak_ptr<utility::Controller::Executor> /*weak_executor*/,
		int depth_freq,
		int depth_start,
		int depth_stop);

	auto scip_exec(SCIP* scip, SCIP_HEUR* heur, SCIP_HEURTIMING heurtiming, SCIP_Bool nodeinfeasible, SCIP_RESULT* result)
		-> SCIP_RETCODE override;

private:
	std::weak_ptr<utility::Controller::Executor> weak_executor;
};

}  // namespace

/****************************
 *  Definition of Scimpl  *
 ****************************/

void ScipDeleter::operator()(SCIP* ptr) {
	scip::call(SCIPfree, &ptr);
}

namespace {

std::unique_ptr<SCIP, ScipDeleter> create_scip() {
	SCIP* scip_raw;
	scip::call(SCIPcreate, &scip_raw);
	std::unique_ptr<SCIP, ScipDeleter> scip_ptr = nullptr;
	scip_ptr.reset(scip_raw);
	return scip_ptr;
}

}  // namespace

scip::Scimpl::Scimpl() : m_scip{create_scip()} {}

Scimpl::Scimpl(Scimpl&&) noexcept = default;

Scimpl::Scimpl(std::unique_ptr<SCIP, ScipDeleter>&& scip_ptr) noexcept : m_scip(std::move(scip_ptr)) {}

Scimpl::~Scimpl() = default;

SCIP* scip::Scimpl::get_scip_ptr() noexcept {
	return m_scip.get();
}

scip::Scimpl scip::Scimpl::copy() const {
	if (m_scip == nullptr) {
		return {nullptr};
	}
	if (SCIPgetStage(m_scip.get()) == SCIP_STAGE_INIT) {
		return {create_scip()};
	}
	auto dest = create_scip();
	// Copy operation is not thread safe
	static auto m = std::mutex{};
	auto g = std::lock_guard{m};
	scip::call(SCIPcopy, m_scip.get(), dest.get(), nullptr, nullptr, "", true, false, false, false, nullptr);
	return {std::move(dest)};
}

scip::Scimpl scip::Scimpl::copy_orig() const {
	if (m_scip == nullptr) {
		return {nullptr};
	}
	if (SCIPgetStage(m_scip.get()) == SCIP_STAGE_INIT) {
		return {create_scip()};
	}
	auto dest = create_scip();
	// Copy operation is not thread safe
	static auto m = std::mutex{};
	auto g = std::lock_guard{m};
	scip::call(SCIPcopyOrig, m_scip.get(), dest.get(), nullptr, nullptr, "", false, false, false, nullptr);
	return {std::move(dest)};
}

void Scimpl::solve_iter_start_branch() {
	auto* const scip_ptr = get_scip_ptr();
	m_controller =
		std::make_unique<utility::Controller>([scip_ptr](std::weak_ptr<utility::Controller::Executor> weak_executor) {
			scip::call(
				SCIPincludeObjBranchrule,
				scip_ptr,
				new ReverseBranchrule(scip_ptr, std::move(weak_executor)),  // NOLINT
				true);
			scip::call(SCIPsolve, scip_ptr);  // NOLINT
		});

	m_controller->wait_executor();
}

void scip::Scimpl::solve_iter_branch(SCIP_RESULT result) {
	m_controller->resume_executor(result);
	m_controller->wait_executor();
}

SCIP_HEUR* Scimpl::solve_iter_start_primalsearch(int depth_freq, int depth_start, int depth_stop) {
	auto* const scip_ptr = get_scip_ptr();
	m_controller = std::make_unique<utility::Controller>([=](std::weak_ptr<utility::Controller::Executor> weak_executor) {
		scip::call(
			SCIPincludeObjHeur,
			scip_ptr,
			new ReverseHeur(scip_ptr, std::move(weak_executor), depth_freq, depth_start, depth_stop),  // NOLINT
			true);
		scip::call(SCIPsolve, scip_ptr);  // NOLINT
	});

	m_controller->wait_executor();
	return SCIPfindHeur(scip_ptr, scip::ReverseHeur::name);
}

void scip::Scimpl::solve_iter_primalsearch(SCIP_RESULT result) {
	m_controller->resume_executor(result);
	m_controller->wait_executor();
}

void scip::Scimpl::solve_iter_stop() {
	m_controller = nullptr;
}

bool scip::Scimpl::solve_iter_is_done() {
	return !(m_controller) || m_controller->is_done();
}

namespace {

/*************************************
 *  Definition of ReverseBranchrule  *
 *************************************/

auto handle_executor(std::weak_ptr<utility::Controller::Executor>& weak_executor, SCIP* scip, SCIP_RESULT* result)
	-> SCIP_RETCODE {
	if (weak_executor.expired()) {
		*result = SCIP_DIDNOTRUN;
		return SCIP_OKAY;
	}
	return std::visit(
		[&](auto result_or_stop) -> SCIP_RETCODE {
			using StopToken = utility::Controller::Executor::StopToken;
			if constexpr (std::is_same_v<decltype(result_or_stop), StopToken>) {
				*result = SCIP_DIDNOTRUN;
				return SCIPinterruptSolve(scip);
			} else {
				*result = result_or_stop;
				return SCIP_OKAY;
			}
		},
		weak_executor.lock()->hold_coroutine());
}

scip::ReverseBranchrule::ReverseBranchrule(SCIP* scip, std::weak_ptr<utility::Controller::Executor> weak_executor_) :
	::scip::ObjBranchrule(
		scip,
		scip::ReverseBranchrule::name,
		"Branchrule that wait for another thread to make the branching.",
		scip::ReverseBranchrule::max_priority,
		scip::ReverseBranchrule::no_maxdepth,
		scip::ReverseBranchrule::no_maxbounddist),
	weak_executor(std::move(weak_executor_)) {}

auto ReverseBranchrule::scip_execlp(
	SCIP* scip,
	SCIP_BRANCHRULE* /*branchrule*/,
	SCIP_Bool /*allowaddcons*/,
	SCIP_RESULT* result) -> SCIP_RETCODE {
	return handle_executor(weak_executor, scip, result);
}

/*******************************
 *  Definition of ReverseHeur  *
 *******************************/

scip::ReverseHeur::ReverseHeur(
	SCIP* scip,
	std::weak_ptr<utility::Controller::Executor> weak_executor_,
	int depth_freq,
	int depth_start,
	int depth_stop) :
	::scip::ObjHeur(
		scip,
		scip::ReverseHeur::name,
		"Primal heuristic that waits for another thread to provide a primal solution.",
		'e',
		scip::ReverseHeur::max_priority,
		depth_freq,
		depth_start,
		depth_stop,
		SCIP_HEURTIMING_AFTERNODE,
		false),
	weak_executor(std::move(weak_executor_)) {}

auto ReverseHeur::scip_exec(
	SCIP* scip,
	SCIP_HEUR* /*heur*/,
	SCIP_HEURTIMING /*heurtiming*/,
	SCIP_Bool /*nodeinfeasible*/,
	SCIP_RESULT* result) -> SCIP_RETCODE {
	return handle_executor(weak_executor, scip, result);
}

}  // namespace
}  // namespace ecole::scip
