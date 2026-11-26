# A send of a computed name on self in an instance method, with a splat,
# two arguments or keywords and a block, does not stop a lambda's return in
# a required file's top level: that self is no class nor main, so the send
# cannot redefine the `lambda` the file calls.
require_relative "computed_send_args_lambda_return_required/codec"

class Box
  def size(n = 0, m = 0, key: 0) = 3 + n + m + key + (block_given? ? yield : 0)
  def ask(name, *args) = send(name, *args)
  def ask2(name) = send(name, 1, 2)
  def ask_kw(name) = send(name, key: 1) { 2 }
  # code in a String no literal spells runs in no compiled program, and a
  # definition form sent to self's own singleton class makes a method of
  # that object alone (none of these is called)
  def eval_on(str) = instance_eval(str)
  def self.add(code) = class_eval(code, __FILE__, __LINE__)
  def bind(key, value) = singleton_class.send(:define_method, key) { value }
  # in a class method self is the class: what a send defines there is the
  # class's, which main does not answer
  def self.make(name, *args) = send(name, *args)
  def self.defn(key) = send(:define_method, key) { 1 }
end

p Box.new.ask(:size, 1)
p Box.new.ask2(:size)
p Box.new.ask_kw(:size)
p Pairs.call([1, 2, 3, 4])
p Pairs.call(7)
