# A class whose superclass is a builtin one answers name, to_s and inspect
# on itself, as a class with no such superclass does (see
# class_builtin_before_override.rb): a call that runs before the program's
# class method of the name gets the builtin, from a method, a lambda and a
# load point, on the class and on a subclass of it; after the def the
# program's method answers. (One def per name in a file, as there: a name
# the program defines twice keeps the table's answer.)

class CA < Array; end
class CB < CA; end
class CH < Hash; end
class CHS < CH; end
class CS < String; end

def array_name = CA.name
def sub_name(k) = k.name
def hash_text = [CH.to_s, CHS.to_s]
def string_text = CS.inspect
array_lambda = -> { [CA.name, CB.name] }
def calls = [array_name, sub_name(CB), sub_name(CA), CA.then { |k| k.name }, hash_text, string_text]
p calls, array_lambda.(), CA.name, CH.to_s, CHS.to_s, CS.inspect

class CA
  def self.name = "ca"
end
class CH
  def self.to_s = "ch"
end
def CS.inspect = "cs"
p calls, array_lambda.(), CA.name, CH.to_s, CHS.to_s, CS.inspect
