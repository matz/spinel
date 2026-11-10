# raise k, m with k a class held in a variable, a parameter, an ivar or
# returned by a call: CRuby raises what k.exception(m) answers, so a
# class with its own (or an inherited) def self.exception raises that.
class WrapErr < StandardError
  def self.exception(*a) = RuntimeError.new("wrapped #{a.inspect}")
end
class WrapKid < WrapErr; end
class PlainErr < StandardError; end
class InitErr < StandardError
  def initialize(m = "dflt") = super("init:#{m}")
end
class PrivErr < StandardError
  class << self
    private
    def exception(m = nil) = ArgumentError.new("priv #{m}")
  end
end

def show
  yield
rescue Exception => e
  p [e.class, e.message]
end

k = WrapErr
show { raise k, "boom" }
show { raise k }
[WrapErr, WrapKid, PlainErr, InitErr, PrivErr, KeyError].each do |c|
  show { raise c, "z" }
  show { raise c }
end

def pick(i)
  puts "pick #{i}"
  [WrapErr, PlainErr][i]
end
show { raise pick(0), "once" }
show { raise pick(1), "once" }

@k = WrapKid
show { raise @k, "ivar" }

def raise_it(c, m) = raise(c, m)
show { raise_it(WrapErr, "param") }
show { fail k, "fail" }

# a value that is not a class keeps its own result
["str", Object.new, 3].each { |v| show { raise v, "x" } }
show { raise PlainErr.new("inst"), "msg" }
