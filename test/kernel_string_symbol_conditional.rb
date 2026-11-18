def convert(value)
  p [String(:abc), String(value)]
end

def boxed
  [:abc, 12][0]
end

convert(boxed)
if ARGV.empty?
  class Symbol
    def to_str
      +"implicit"
    end
  end
end
convert(boxed)
