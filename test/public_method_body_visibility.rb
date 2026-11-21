# public_method of a class method whose visibility a method body's word
# sets reads that visibility where it runs: on a constant, on self and
# with no receiver, and through what the Method answers.
class Ledger
  def self.total = 42
  def self.audit(x, y = 1) = x + y
  def self.lock
    private_class_method :total
    nil
  end
  def self.open
    public_class_method "total", "audit"
    nil
  end
  def self.own = public_method(:total).call
  def self.mine = self.public_method(:audit).arity
end

class Vault
  def self.peek = :peek
  def self.guard
    class << self
      protected :peek
    end
    nil
  end
end

def try = yield rescue [$!.class, $!.message]
def read(tag)
  p [tag, try { Ledger.public_method(:total).call }, try { Ledger.public_method(:audit).owner == Ledger.singleton_class }]
  p [tag, try { Ledger.own }, try { Ledger.mine }, try { Ledger.public_method(:total).unbind.name }]
  p [tag, Ledger.method(:total).call, try { Ledger.public_method(:audit).name }]
end

read(:start)
Ledger.lock
read(:locked)
Ledger.open
read(:opened)
p((Vault.public_method(:peek).name rescue [$!.class, $!.message]))
Vault.guard
p [(Vault.public_method(:peek).name rescue [$!.class, $!.message]), Vault.method(:peek).call]
