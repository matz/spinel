# The same for a class whose superclass is an exception class or a Struct
# class (see class_builtin_sub_before_override.rb), and for a def taking an
# argument, which the builtin does not: the builtin answers a call before
# that def only where it takes the call's arguments.

class CE < StandardError; end
class CF < CE; end
class CT < Struct.new(:a); end

def error_inspect = CE.inspect
def error_sub(k) = k.inspect
def struct_name = CT.name
def struct_to_s = CT.to_s
error_lambda = -> { [CE.inspect, CF.inspect] }
def calls = [error_inspect, error_sub(CF), error_sub(CE), struct_name, struct_to_s]
p calls, error_lambda.(), CE.inspect, CT.name

class CE
  def self.inspect = "ce"
end
class CT
  def self.name = "ct"
end
p calls, error_lambda.(), CE.inspect, CT.name

# A def taking an argument is a method the builtin does not answer; the
# call without one raises ArgumentError after it.
class CV < Array; end
def vv_to_s = CV.to_s
p vv_to_s
class CV
  def self.to_s(x = 1) = "arg#{x}"
end
p vv_to_s, CV.to_s(2)

begin
  raise CE, "boom"
rescue CE => e
  p e.class.name, e.message
end
