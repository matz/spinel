# A builtin module mixed into the including class by a hook:
# Enumerable calls from class methods, receiverless, and through self.class.
module M
  def self.included(base)
    base.class_eval do
      extend Enumerable
    end
  end
end
class A
  include M
  def self.each
    yield 1
    yield 2
    yield 3
  end
  def self.total = sum
  def self.evens = select(&:even?)
  def peers = self.class.to_a
end
p A.total, A.evens, A.new.peers, A.each_slice(2).to_a
