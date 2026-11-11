# An inherited initialize of an exception class writes its ivars through a frozen guard; exception structs carry no cls_id.
class AppError < RuntimeError
  attr_reader :code

  def initialize(code)
    @code = code
    super("failed #{code}")
  end
end

class NotFound < AppError
end

e = NotFound.new(404)
puts e.code
puts e.message
e.freeze
begin
  e.send(:initialize, 500)
rescue FrozenError => ex
  puts ex.class
end
puts e.code
