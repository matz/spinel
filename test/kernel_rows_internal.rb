# A program may define methods named like the compiler's own ENV rows;
# ENV still reads ENV, and the program's own call of such a name is its own.
def __env_value = :top
def __env_to_h = :top
ENV["SPK_KR1"] = "v"
x = ENV
p x["SPK_KR1"]
p ENV.keys.include?("SPK_KR1")
p ENV.to_h["SPK_KR1"]
p __env_value
p __env_to_h

class K
  def self.__env_to_h = :cm
  def __env_value = :im
  def self.go = ENV.to_h["SPK_KR1"]
  def go2 = (y = ENV; y["SPK_KR1"])
  def mine = __env_value
end
p K.go
p K.new.go2
p K.new.mine

class Object
  def __env_value = :obj
  def __env_to_h = :obj
end
z = ENV
p z["SPK_KR1"]
p ENV.to_h["SPK_KR1"]
p 1.__env_value
