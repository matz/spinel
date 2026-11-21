# public_method finds only public methods: a private or protected one,
# on the instance side or the class side, raises NameError naming the
# receiver's class; method finds it whatever its visibility.
class Account
  def self.open = :open
  def self.audit = :audit
  private_class_method :audit
  def balance = 10
  def secret = :secret
  private :secret
  def ledger = :ledger
  protected :ledger
end
class Savings < Account; end

def try = yield rescue [$!.class, $!.message]
a = Account.new
p try { Account.public_method(:open).call }
p try { Account.public_method(:audit) }
p try { Savings.public_method(:audit) }
p try { a.public_method(:balance).call }
p try { a.public_method(:secret) }
p try { a.public_method(:ledger) }
p try { Savings.new.public_method(:secret) }
p [a.method(:secret).call, Account.method(:audit).call]
