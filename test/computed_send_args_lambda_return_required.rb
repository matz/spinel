# Sends of a computed name, on any receiver, and evals of code in a String
# do not stop a lambda's return in a required file's top level: redefining
# or removing the builtin `lambda` that way is not supported.
require_relative "computed_send_args_lambda_return_required/codec"

class Box
  def size(n = 0, m = 0, key: 0) = 3 + n + m + key + (block_given? ? yield : 0)
  def ask(name, *args) = send(name, *args)
  def ask2(name) = send(name, 1, 2)
  def ask_kw(name) = send(name, key: 1) { 2 }
  # (none of these is called)
  def eval_on(str) = instance_eval(str)
  def self.add(code) = class_eval(code, __FILE__, __LINE__)
  def bind(key, value) = singleton_class.send(:define_method, key) { value }
  def self.make(name, *args) = send(name, *args)
  def self.defn(key) = send(:define_method, key) { 1 }
end

class Relay
  def initialize(to) = @to = to
  def ask(name, *args) = @to.send(name, *args)
  def ask_pub(name, a, b) = @to.public_send(name, a, b)
end

p Box.new.ask(:size, 1)
p Box.new.ask2(:size)
p Box.new.ask_kw(:size)
p Relay.new(Box.new).ask(:size, 2)
p Relay.new(Box.new).ask_pub(:size, 1, 1)
p Pairs.call([1, 2, 3, 4])
p Pairs.call(7)
