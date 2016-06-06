 /*
 *  Created by Phil on 22/10/2010.
 *  Copyright 2010 Two Blue Cubes Ltd. All rights reserved.
 *
 *  Distributed under the Boost Software License, Version 1.0. (See accompanying
 *  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 */
#ifndef TWOBLUECUBES_CATCH_RUNNER_IMPL_HPP_INCLUDED
#define TWOBLUECUBES_CATCH_RUNNER_IMPL_HPP_INCLUDED

#include "catch_thread_context.hpp"

#include "catch_interfaces_capture.h"
#include "catch_interfaces_reporter.h"
#include "catch_interfaces_exception.h"
#include "catch_config.hpp"
#include "catch_test_registry.hpp"
#include "catch_test_case_info.h"
#include "catch_capture.hpp"
#include "catch_totals.hpp"
#include "catch_test_spec.hpp"
#include "catch_test_case_tracker.hpp"
#include "catch_timer.h"
#include "catch_result_builder.h"
#include "catch_fatal_condition.hpp"

#include <set>
#include <string>

namespace Catch {

    class StreamRedirect {

    public:
        StreamRedirect( std::ostream& stream, std::string& targetString )
        :   m_stream( stream ),
            m_prevBuf( stream.rdbuf() ),
            m_targetString( targetString )
        {
            stream.rdbuf( m_oss.rdbuf() );
        }

        ~StreamRedirect() {
            m_targetString += m_oss.str();
            m_stream.rdbuf( m_prevBuf );
        }

    private:
        std::ostream& m_stream;
        std::streambuf* m_prevBuf;
        std::ostringstream m_oss;
        std::string& m_targetString;
    };

    ///////////////////////////////////////////////////////////////////////////

    namespace {
        IRunContext* g_globalRunContext = CATCH_NULL;

        void setGlobalRunContext( IRunContext* context ) {
            assert( g_globalRunContext == CATCH_NULL || context == CATCH_NULL );
            g_globalRunContext = context;
        }
    }
    
    IRunContext* tryGetGlobalRunContext() {
        return g_globalRunContext;
    }
    IRunContext& getGlobalRunContext() {
        if( IRunContext* capture = tryGetGlobalRunContext() )
            return *capture;
        else
            throw std::logic_error( "No current test runner" );
    }
    IConfig const* getGlobalConfig() {
        if( IRunContext* capture = tryGetGlobalRunContext() )
            return &capture->config();
        else
            return CATCH_NULL;
    }

    class RunContext : public IRunContext {

        RunContext( RunContext const& );
        void operator =( RunContext const& );

    public:

        explicit RunContext( Ptr<IConfig const> const& _config, Ptr<IStreamingReporter> const& reporter )
        :   m_runInfo( _config->name() ),
            m_config( _config ),
            m_reporter( reporter ),
            m_activeTestCaseInfo( CATCH_NULL )
        {
            setGlobalRunContext( this );
            m_reporter->testRunStarting( m_runInfo );
        }

        virtual ~RunContext() {
            m_reporter->testRunEnded( TestRunStats( m_runInfo, m_totals, isAborting() ) );
            setGlobalRunContext( CATCH_NULL );
        }

        void testGroupStarting( std::string const& testSpec, std::size_t groupIndex, std::size_t groupsCount ) {
            m_reporter->testGroupStarting( GroupInfo( testSpec, groupIndex, groupsCount ) );
        }
        void testGroupEnded( std::string const& testSpec, Totals const& totals, std::size_t groupIndex, std::size_t groupsCount ) {
            m_reporter->testGroupEnded( TestGroupStats( GroupInfo( testSpec, groupIndex, groupsCount ), totals, isAborting() ) );
        }

        Totals runTest( TestCase const& testCase ) {
            m_activeTestCaseInfo = &testCase;

            Totals prevTotals = m_totals;
            std::string redirectedCout, redirectedCerr;

            m_reporter->testCaseStarting( testCase );

            ITracker* m_testCaseTracker;

            m_trackerContext.startRun();
            do {
                m_trackerContext.startCycle();
                m_testCaseTracker = &SectionTracker::acquire( m_trackerContext, testCase.name );
                runTest( testCase, redirectedCout, redirectedCerr );
            }
            while( !m_testCaseTracker->isSuccessfullyCompleted() && !isAborting() );

            
            Totals deltaTotals = m_totals.delta( prevTotals );
            m_totals.testCases += deltaTotals.testCases;
            m_reporter->testCaseEnded( TestCaseStats(   testCase,
                                                        deltaTotals,
                                                        redirectedCout,
                                                        redirectedCerr,
                                                        isAborting() ) );

            m_activeTestCaseInfo = CATCH_NULL;

            return deltaTotals;
        }

    private: // IRunContext


        virtual void assertionEnded( AssertionResult const& result ) CATCH_OVERRIDE {
            if( result.getResultType() == ResultWas::Ok )
                m_totals.assertions.passed++;
            else if( !result.isOk() )
                m_totals.assertions.failed++;

            if( m_reporter->assertionEnded( AssertionStats( result, m_messages, m_totals ) ) )
                m_messages.clear();

            // Reset working state
            m_lastAssertionInfo = AssertionInfo( "", m_lastAssertionInfo.lineInfo, "{Unknown expression after the reported line}" , m_lastAssertionInfo.resultDisposition );
            m_lastResult = result;
        }

        virtual bool sectionStarted
            (   SectionInfo const& sectionInfo,
                Counts& assertions ) CATCH_OVERRIDE
        {
            std::ostringstream oss;
            oss << sectionInfo.name << "@" << sectionInfo.lineInfo;

            ITracker& sectionTracker = SectionTracker::acquire( m_trackerContext, oss.str() );
            if( !sectionTracker.isOpen() )
                return false;
            m_activeSections.push_back( &sectionTracker );

            m_lastAssertionInfo.lineInfo = sectionInfo.lineInfo;

            m_reporter->sectionStarting( sectionInfo );

            assertions = m_totals.assertions;

            return true;
        }
        bool testForMissingAssertions( Counts& assertions ) {
            if( assertions.total() != 0 )
                return false;
            if( !m_config->warnAboutMissingAssertions() )
                return false;
            if( m_trackerContext.currentTracker().hasChildren() )
                return false;
            m_totals.assertions.failed++;
            assertions.failed++;
            return true;
        }

        virtual void sectionEnded( SectionEndInfo const& endInfo ) CATCH_OVERRIDE {
            Counts assertions = m_totals.assertions - endInfo.prevAssertions;
            bool missingAssertions = testForMissingAssertions( assertions );

            if( !m_activeSections.empty() ) {
                m_activeSections.back()->close();
                m_activeSections.pop_back();
            }

            m_reporter->sectionEnded( SectionStats( endInfo.sectionInfo, assertions, endInfo.durationInSeconds, missingAssertions ) );
            m_messages.clear();
        }

        virtual void sectionEndedEarly( SectionEndInfo const& endInfo ) CATCH_OVERRIDE {
            if( m_unfinishedSections.empty() )
                m_activeSections.back()->fail();
            else
                m_activeSections.back()->close();
            m_activeSections.pop_back();

            m_unfinishedSections.push_back( endInfo );
        }

        virtual void pushScopedMessage( MessageInfo const& message ) CATCH_OVERRIDE {
            m_messages.push_back( message );
        }

        virtual void popScopedMessage( MessageInfo const& message ) CATCH_OVERRIDE {
            m_messages.erase( std::remove( m_messages.begin(), m_messages.end(), message ), m_messages.end() );
        }

        virtual std::string getCurrentTestName() const CATCH_OVERRIDE {
            return m_activeTestCaseInfo
                ? m_activeTestCaseInfo->name
                : "";
        }

        virtual AssertionResult const* getLastResult() const CATCH_OVERRIDE {
            return &m_lastResult;
        }
        virtual IConfig const& config() const CATCH_OVERRIDE {
            return *m_config;
        }

        virtual void handleFatalErrorCondition( std::string const& message ) CATCH_OVERRIDE {
            ResultBuilder resultBuilder = makeUnexpectedResultBuilder();
            resultBuilder.setResultType( ResultWas::FatalErrorCondition );
            resultBuilder << message;
            resultBuilder.captureExpression();

            handleUnfinishedSections();

            // Recreate section for test case (as we will lose the one that was in scope)
            SectionInfo testCaseSection
                (   C_A_T_C_H_Context(),
                    m_activeTestCaseInfo->lineInfo,
                    m_activeTestCaseInfo->name,
                    m_activeTestCaseInfo->description );

            Counts assertions;
            assertions.failed = 1;
            SectionStats testCaseSectionStats( testCaseSection, assertions, 0, false );
            m_reporter->sectionEnded( testCaseSectionStats );

            Totals deltaTotals;
            deltaTotals.testCases.failed = 1;
            m_reporter->testCaseEnded( TestCaseStats(   *m_activeTestCaseInfo,
                                                        deltaTotals,
                                                        "",
                                                        "",
                                                        false ) );
            m_totals.testCases.failed++;
            testGroupEnded( "", m_totals, 1, 1 );
            m_reporter->testRunEnded( TestRunStats( m_runInfo, m_totals, false ) );
        }

    public:
        // !TBD We need to do this another way!
        bool isAborting() const {
            return m_totals.assertions.failed == static_cast<std::size_t>( m_config->abortAfter() );
        }

    private:

        void runTest( TestCase const& testCase, std::string& redirectedCout, std::string& redirectedCerr ) {
            SectionInfo testCaseSection( *this, testCase.lineInfo, testCase.name, testCase.description );
            m_reporter->sectionStarting( testCaseSection );
            Counts prevAssertions = m_totals.assertions;
            double duration = 0;
            try {
                m_lastAssertionInfo = AssertionInfo( "TEST_CASE", testCase.lineInfo, "", ResultDisposition::Normal );

                seedRng( *m_config );

                Timer timer;
                timer.start();
                if( m_reporter->getPreferences().shouldRedirectStdOut ) {
                    StreamRedirect coutRedir( Catch::cout(), redirectedCout );
                    StreamRedirect cerrRedir( Catch::cerr(), redirectedCerr );
                    invokeTestCase( testCase );
                }
                else {
                    invokeTestCase( testCase );
                }
                duration = timer.getElapsedSeconds();
            }
            catch( TestFailureException& ) {
                // This just means the test was aborted due to failure
            }
            catch(...) {
                makeUnexpectedResultBuilder().useActiveException();
            }
            m_trackerContext.currentTracker().close();
            
            handleUnfinishedSections();
            m_messages.clear();

            Counts assertions = m_totals.assertions - prevAssertions;
            bool missingAssertions = testForMissingAssertions( assertions );

            if( testCase.okToFail() ) {
                std::swap( assertions.failedButOk, assertions.failed );
                m_totals.assertions.failed -= assertions.failedButOk;
                m_totals.assertions.failedButOk += assertions.failedButOk;
            }

            SectionStats testCaseSectionStats( testCaseSection, assertions, duration, missingAssertions );
            m_reporter->sectionEnded( testCaseSectionStats );
        }

        static void invokeTestCase( TestCase const& testCase ) {
            FatalConditionHandler fatalConditionHandler; // Handle signals
            testCase.invoke();
            fatalConditionHandler.reset(); // Not strictly needed but avoids warnings
        }

    private:

        ResultBuilder makeUnexpectedResultBuilder() {
            return ResultBuilder
                (   *this,
                    m_lastAssertionInfo.macroName.c_str(),
                    m_lastAssertionInfo.lineInfo,
                    m_lastAssertionInfo.capturedExpression.c_str(),
                    m_lastAssertionInfo.resultDisposition );
        }

        void handleUnfinishedSections() {
            // If sections ended prematurely due to an exception we stored their
            // infos here so we can tear them down outside the unwind process.
            for( std::vector<SectionEndInfo>::const_reverse_iterator it = m_unfinishedSections.rbegin(),
                        itEnd = m_unfinishedSections.rend();
                    it != itEnd;
                    ++it )
                sectionEnded( *it );
            m_unfinishedSections.clear();
        }

        TestRunInfo m_runInfo;

        Ptr<IConfig const> m_config;
        Ptr<IStreamingReporter> m_reporter;
        TrackerContext m_trackerContext;
        Totals m_totals;
        
        // Transient state
        TestCaseInfo const* m_activeTestCaseInfo;
        AssertionResult m_lastResult;
        AssertionInfo m_lastAssertionInfo;
        std::vector<SectionEndInfo> m_unfinishedSections;
        std::vector<ITracker*> m_activeSections;
        std::vector<MessageInfo> m_messages;
    };

} // end namespace Catch

#endif // TWOBLUECUBES_CATCH_RUNNER_IMPL_HPP_INCLUDED
