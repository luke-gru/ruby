require 'mspec/runner/mspec'

# Holds some of the state of the example (i.e. +it+ block) that is
# being evaluated. See also +ContextState+.
class ExampleState
  attr_reader :context, :it, :example

  def initialize(context, it, example = nil)
    @context = context
    @it = it
    @example = example # proc
    if @example
      $stderr.puts RUBY_DESCRIPTION
      block_lines = RubyVM::InstructionSequence.of(example).script_lines
      if block_lines
        $stderr.puts "block_lines found"
        exit 1
        block_lines.unshift "Ractor.new do\n"
        block_lines << "end.take\n"
        @example = eval "proc do\n#{block_lines.join}\nend"
      end
    end
  end

  def context=(context)
    @description = nil
    @context = context
  end

  def describe
    @context.description
  end

  def description
    @description ||= "#{describe} #{@it}"
  end

  def filtered?
    incl = MSpec.include
    excl = MSpec.exclude
    included   = incl.empty? || incl.any? { |f| f === description }
    included &&= excl.empty? || !excl.any? { |f| f === description }
    !included
  end
end
