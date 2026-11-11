# A super in a reopened exception's message reaches the version of the base
# reopening's message that is live at the super, for a raised exception.
class StandardError
  def message = "SE1"
end
class TypeError
  def message = "TE:" + super
end
def go
  raise TypeError, "t"
rescue => e
  puts e.message
end
go
class StandardError
  def message = "SE2"
end
go
