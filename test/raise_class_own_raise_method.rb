# A program's own class-method `raise` (or a module function) takes the
# class and the message as given, even when the class has its own
# self.exception.
class AppError < StandardError; end
class W < StandardError
  def self.exception(m = "d") = AppError.new("w:" + m)
  def self.raise(*a) = p(a)
  def self.go = raise(W, "c")
end
W.go
module Util; module_function; def raise(*a) = p(a); def go = raise(W, "u"); end
Util.go
class Q; def self.fail(*a) = p(a); def self.go = fail(W, "f"); end
Q.go
