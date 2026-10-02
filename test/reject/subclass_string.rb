# A subclass of String: the constructor took none of String's arguments and
# raised ArgumentError at run time (#7075).
class Name < String
end

p Name.new("hi").upcase
