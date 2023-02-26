# frozen_string_literal: true
require_relative 'assertions'
require_relative '../../core_assertions'

module Test
  module Unit

    ##
    # Provides a simple set of guards that you can use in your tests
    # to skip execution if it is not applicable. These methods are
    # mixed into TestCase as both instance and class methods so you
    # can use them inside or outside of the test methods.
    #
    #   def test_something_for_mri
    #     skip "bug 1234"  if jruby?
    #     # ...
    #   end
    #
    #   if windows? then
    #     # ... lots of test methods ...
    #   end

    module Guard

      ##
      # Is this running on jruby?

      def jruby? platform = RUBY_PLATFORM
        "java" == platform
      end

      ##
      # Is this running on mri?

      def mri? platform = RUBY_DESCRIPTION
        /^ruby/ =~ platform
      end

      ##
      # Is this running on windows?

      def windows? platform = RUBY_PLATFORM
        /mswin|mingw/ =~ platform
      end

      ##
      # Is this running on mingw?

      def mingw? platform = RUBY_PLATFORM
        /mingw/ =~ platform
      end

    end

    ##
    # Provides before/after hooks for setup and teardown. These are
    # meant for library writers, NOT for regular test authors. See
    # #before_setup for an example.

    module LifecycleHooks
      ##
      # Runs before every test, after setup. This hook is meant for
      # libraries to extend Test::Unit. It is not meant to be used by
      # test developers.
      #
      # See #before_setup for an example.

      def after_setup; end

      ##
      # Runs before every test, before setup. This hook is meant for
      # libraries to extend Test::Unit. It is not meant to be used by
      # test developers.
      #
      # As a simplistic example:
      #
      #   module MyTestUnitPlugin
      #     def before_setup
      #       super
      #       # ... stuff to do before setup is run
      #     end
      #
      #     def after_setup
      #       # ... stuff to do after setup is run
      #       super
      #     end
      #
      #     def before_teardown
      #       super
      #       # ... stuff to do before teardown is run
      #     end
      #
      #     def after_teardown
      #       # ... stuff to do after teardown is run
      #       super
      #     end
      #   end
      #
      #   class Test::Unit::Runner::TestCase
      #     include MyTestUnitPlugin
      #   end

      def before_setup; end

      ##
      # Runs after every test, before teardown. This hook is meant for
      # libraries to extend Test::Unit. It is not meant to be used by
      # test developers.
      #
      # See #before_setup for an example.

      def before_teardown; end

      ##
      # Runs after every test, after teardown. This hook is meant for
      # libraries to extend Test::Unit. It is not meant to be used by
      # test developers.
      #
      # See #before_setup for an example.

      def after_teardown; end
    end

    ##
    # Subclass TestCase to create your own tests. Typically you'll want a
    # TestCase subclass per implementation class.
    #
    # See <code>Test::Unit::AssertionFailedError</code>s

    class TestCase
      include Assertions
      include CoreAssertions

      include LifecycleHooks
      include Guard
      extend Guard

      attr_reader :__name__ # :nodoc:

      PASSTHROUGH_EXCEPTIONS = [NoMemoryError, SignalException,
                                Interrupt, SystemExit] # :nodoc:

      ##
      # Runs the tests reporting the status to +runner+

      def run runner
        @options = runner.options

        trap "INFO" do
          runner.report.each_with_index do |msg, i|
            warn "\n%3d) %s" % [i + 1, msg]
          end
          warn ''
          time = runner.start_time ? Time.now - runner.start_time : 0
          warn "Current Test: %s#%s %.2fs" % [self.class, self.__name__, time]
          runner.status $stderr
        end if runner.info_signal

        start_time = Time.now

        result = ""

        begin
          @passed = nil
          self.before_setup
          self.setup
          self.after_setup
          self.run_test self.__name__
          result = "." unless io?
          time = Time.now - start_time
          runner.record self.class, self.__name__, self._assertions, time, nil
          @passed = true
        rescue *PASSTHROUGH_EXCEPTIONS
          raise
        rescue Exception => e
          @passed = Test::Unit::PendedError === e
          time = Time.now - start_time
          runner.record self.class, self.__name__, self._assertions, time, e
          result = runner.puke self.class, self.__name__, e
        ensure
          %w{ before_teardown teardown after_teardown }.each do |hook|
            begin
              self.send hook
            rescue *PASSTHROUGH_EXCEPTIONS
              raise
            rescue Exception => e
              @passed = false
              runner.record self.class, self.__name__, self._assertions, time, e
              result = runner.puke self.class, self.__name__, e
            end
          end
          trap 'INFO', 'DEFAULT' if runner.info_signal
        end
        result
      end

      RUN_TEST_TRACE = "#{__FILE__}:#{__LINE__+3}:in `run_test'".freeze
      def run_test(name)
        progname, $0 = $0, "#{$0}: #{self.class}##{name}"
        self.__send__(name)
      ensure
        $@.delete(RUN_TEST_TRACE) if $@
        $0 = progname
      end

      def initialize name # :nodoc:
        @__name__ = name
        @__io__ = nil
        @passed = nil
        @@current = self # FIX: make thread local
      end

      def self.current # :nodoc:
        @@current # FIX: make thread local
      end

      def self.reset_current # nodoc:
        @@current = nil
      end

      ##
      # Return the output IO object

      def io
        @__io__ = true
        Test::Unit::Runner.output
      end

      ##
      # Have we hooked up the IO yet?

      def io?
        @__io__
      end

      def self.reset # :nodoc:
        @@test_suites = {}
        @@test_suites[self] = true
      end

      reset

      @@uncollectible_suites = []
      @@collected_suites = []

      def self.inherited klass # :nodoc:
        @@test_suites[klass] = true
        # If test class x gets inherited by other test classes, we don't collect x
        if self != Test::Unit::TestCase
          @@uncollectible_suites << self.name if self.name && !@@uncollectible_suites.include?(self.name)
        end
        # if:
        # class A < Test::Unit::TestCase
        #   class B < Test::Unit::TestCase; end
        # end
        # then: upon seeing class B, we mark A as uncollectible, since B could run after A
        # and we need to get B from its full class name before running it.
        _klass = klass
        while _klass
          if _klass.name && _klass.name.index('::')
            parent_name = _klass.name[0..._klass.name.rindex('::')]
            parent_mod = Object.const_get(parent_name)
            if parent_mod.ancestors.include?(Test::Unit::TestCase)
              @@uncollectible_suites << parent_name unless @@uncollectible_suites.include?(parent_name)
              #$stderr.puts "Skipping collecting #{parent_name}"
            end
            _klass = parent_mod
          else
            break
          end
        end
        super
      end

      @test_order = :sorted

      class << self
        attr_writer :test_order
        def _collected_suites
          @@collected_suites
        end
        def _uncollectible_suites
          @@uncollectible_suites
        end
        def _make_collectible(suite)
          @@test_suites.delete(suite)
        end
        def make_uncollectible!
          return unless self.name
          @@uncollectible_suites << self.name unless @@uncollectible_suites.include?(self.name)
        end
        alias old_const_missing const_missing
        def const_missing(name)
          Test::Unit::TestCase.collected_warning(name)
          old_const_missing(name)
        end
        def collected_warning(name)
          @_collected_warnings ||= Hash.new { |h,k| h[k] = {} }
          @_collected_warnings.delete_if { |k,v| k != @@current.hash }
          name = name.to_s
          if _collected_suites.include?(name) && !@_collected_warnings[@@current.hash][name]
            $stderr.puts "Error: class #{name} has been collected (test suite GC). Consider:\n" +
              "class #{name}\n" +
              "  self.make_uncollectible!\n" +
              "end"
            # don't output same message more than once in same testcase
            @_collected_warnings[@@current.hash][name] = true
          end
        end
      end

      class ::Module # :nodoc: all
        EnvUtil.suppress_warning do # method redefined
          alias included_without_collection_warning included
          # Give good error messages when getting NameError for collected
          # suites, or constants within collected suites.
          # This catches the following case, and gives a warning that MyTest
          # has been collected.
=begin
            class MyTest < Test::Unit::TestCase
               ...
            end
            class MyTest2 < Test::Unit::TestCase
              module M
                def test_mine
                  # MyTest could be GC'd
                  assert MyTest::Error.ancestors.include?(StandardError)
                end
              end
              include M
            end
=end
          # The reason we don't redefine Module#const_missing is because some tests rely
          # on it not being overridden. So we only override it for modules
          # that are included in the test classes.
          def included(mod)
            if frozen?
              return included_without_collection_warning(mod)
            end
            @_const_missing_with_collect_warning ||= nil
            if mod.is_a?(Class) && mod.ancestors.include?(Test::Unit::TestCase) &&
                @_const_missing_with_collect_warning.nil?
              orig = self.method(:const_missing)
              define_singleton_method(:const_missing) do |name|
                Test::Unit::TestCase.collected_warning(name)
                orig.call(name)
              end
              @_const_missing_with_collect_warning = true
            end
            included_without_collection_warning(mod)
          end
        end
      end

      def self.test_order
        defined?(@test_order) ? @test_order : superclass.test_order
      end

      def self.test_suites # :nodoc:
        @@test_suites.keys
      end

      def self.test_methods # :nodoc:
        public_instance_methods(true).grep(/^test/)
      end

      ##
      # Returns true if the test passed.

      def passed?
        @passed
      end

      ##
      # Runs before every test. Use this to set up before each test
      # run.

      def setup; end

      ##
      # Runs after every test. Use this to clean up after each test
      # run.

      def teardown; end

      def on_parallel_worker?
        false
      end

      def self.method_added(name)
        super
        return unless name.to_s.start_with?("test_")
        @test_methods ||= {}
        if @test_methods[name]
          raise AssertionFailedError, "test/unit: method #{ self }##{ name } is redefined"
        end
        @test_methods[name] = true
      end
    end
  end
end
