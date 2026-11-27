class Greeter
  alias quiet_greet greet

  def greet
    quiet_greet.upcase
  end
end
