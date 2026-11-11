# A super in a subclass's to_s of a builtin exception reaches the version of the
# reopening's to_s that is live at the super: the second call runs after
# StandardError#to_s is defined again.
class StandardError
  def to_s = "SE1"
end
class ArgumentError
  def to_s = "AE:" + super
end
def go
  raise ArgumentError, "q"
rescue => e
  puts e.to_s
end
go
class StandardError
  def to_s = "SE2"
end
go
