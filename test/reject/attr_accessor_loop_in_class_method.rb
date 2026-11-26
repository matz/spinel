# attr_accessor in a class method's loop over its parameters: the name is
# known only at run time
class Base
  def self.identified_by(*names)
    names.each { |name| attr_accessor name }
  end
end

class Conn < Base
  identified_by :user, :device
end

c = Conn.new
c.user = 1
p c.user
