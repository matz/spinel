# A method named zip on an unrelated class leaves Array#zip available.
class Other
  def zip
    :other
  end
end
p Other.new.zip
[1, 2].zip([], []) { |tuple| p tuple }
