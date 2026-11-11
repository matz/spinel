# `self.a = self.b = nil` where b's writer is only ever handed nil: the inner
# assignment's value is nil, which no temporary holds (its C type is void).
class Account
  attr_reader :password

  def password=(value)
    @password = value
  end

  def password_confirmation=(value)
    @password_confirmation = value
  end

  def password_confirmation
    @password_confirmation
  end

  def clean_up_passwords
    self.password = self.password_confirmation = nil
  end

  def cleared
    result = (self.password = self.password_confirmation = nil)
    [result, password, password_confirmation]
  end
end

account = Account.new
account.password = "secret"
p account.clean_up_passwords
p account.password
p account.password_confirmation
account.password = "again"
p account.cleared
