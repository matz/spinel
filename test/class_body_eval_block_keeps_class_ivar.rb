# A module's included hook shapes the includer through `base.class_eval do
# ... end`, writing a class-level ivar a `class << self` reader answers. The
# program also gives Kernel a class_eval of its own that takes a block, and
# runs a stored proc through instance_exec somewhere: the block of the
# spliced class_eval still runs on the class it was spliced into, so its
# ivar is the class's own slot, the one the reader reads.
module Kernel
  def class_eval(*args, &block)
    singleton_class.class_eval(*args, &block)
  end
end

class Runner
  def initialize; @hooks = {}; end
  def on(&b); @hooks[:k] = b; end
  def run(obj); obj.instance_exec(&@hooks[:k]); end
end

module Parameters
  DEFAULT_PARSERS = { json: ->(raw) { raw.size } }.freeze

  def self.included(base)
    base.extend(Parameters::ClassMethods)
    base.class_eval do
      class << self
        attr_reader :parameter_parsers
      end

      @parameter_parsers = DEFAULT_PARSERS
    end
  end

  module ClassMethods
    def default_parser_key = :json
  end

  def parsers_count
    params_parsers.size
  end

  private
    def params_parsers
      Request.parameter_parsers
    end
end

module Negotiation
  def self.included(base)
    base.class_eval do
      class << self
        attr_accessor :ignore_accept_header
      end
      self.ignore_accept_header = false
    end
  end
end

class Request
  include Negotiation
  include Parameters
  def initialize(env); @env = env; end
end

p Request.parameter_parsers.keys
r = Request.new({})
p r.parsers_count
p Request.ignore_accept_header
p Request.default_parser_key
rn = Runner.new
rn.on { @env.size }
p rn.run(r)
