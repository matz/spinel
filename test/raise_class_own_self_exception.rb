# `raise K, m` raises what K.exception(m) answers, when K (or a class
# above it) defines its own class method `exception`.
class AppError < StandardError; end
class W < StandardError
  def self.exception(m = "dflt") = AppError.new("app:" + m)
end
class W2 < W; end
class Plain < StandardError; end
class V < StandardError
  class << self
    def exception(*a) = super(*a)
  end
end
module Nest
  class X < StandardError; def self.exception(m) = RuntimeError.new("nx " + m); end
end
def try
  yield
rescue => e
  p [e.class, e.message]
end
try { raise W, "r" }
try { raise W }
try { raise W2, "sub" }
try { fail W, "f" }
try { raise Plain, "p" }
try { raise Plain }
try { raise V, "v" }
try { raise Nest::X, "q" }
try { raise W, "c", cause: nil }
begin
  raise W, "r2"
rescue AppError => e
  p [:app, e.message]
end
def m2 = raise(W, "inmeth")
try { m2 }
x = begin; raise W, "val"; rescue => e; e; end
p x.class
class P < StandardError
  def self.exception(m = "d") = AppError.new("p:" + m)
  private_class_method :exception
end
try { raise P, "priv" }
class In < StandardError
  def self.exception(m = "d") = AppError.new("in:" + m)
  def self.go = raise(In, "self")
end
try { In.go }
class Sup < StandardError; def self.exception(m = "d") = AppError.new("sup:" + m); end
class Sub < Sup; def self.exception(m = "d") = super("sub" + m); end
try { raise Sub, "x" }
class Top < StandardError; def self.exception(m = "d") = AppError.new("top:" + m); end
class Mid < Top; def self.exception(m = "d") = super("mid:" + m); end
class Low < Mid; def self.exception(m = "d") = super("low:" + m); end
try { raise Low, "x" }
