# An exception class's #to_s override is kept, with the class method it
# calls, once an instance of the class can exist, and a class nothing builds
# keeps neither (#11625): the text dispatcher has no arm for it.
# absent: sp_Unbuilt_to_s sp_Unbuilt_s_label
# present: sp_Built_to_s sp_Built_s_label
class Unbuilt < StandardError
  def to_s
    self.class.label
  end
  def self.label
    "unbuilt"
  end
end
class Built < StandardError
  def to_s
    self.class.label
  end
  def self.label
    "built"
  end
end
begin
  raise Built
rescue => e
  puts e.message
end
