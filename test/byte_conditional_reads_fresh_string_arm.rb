# A conditional whose value is a String's bytes reads an arm the
# representation holds as a handle (a fresh String: a format call, an
# interpolation) through the String face: SP_STR_B, which reads a handle and
# keeps bytes. It read it through sp_strbuf_read, a handle reader, and an arm
# that rendered as bytes did not compile in a large application program's
# YAML emitter (`put(o <= 0xFF ? format("x%02X", o) : o <= 0xFFFF ?
# format("u%04X", o) : format("U%08X", o))`, the inner conditional's arms) and
# in its URI builder (`path = cond ? "/%2F#{path[1..-1]}" : "/#{path}"`).
class Emitter
  DOUBLE_ESCAPES = { "\n" => "n", "\t" => "t" }
  def initialize
    @out = +""
    @last = +""
    @column = 0
  end
  def put(str)
    @out << str
    @column += str.length
    @last = str
  end
  def mark! = @last << "!"
  def indent(ind)
    put(" " * (ind - @column)) if @column < ind
  end
  def escape(str)
    str.each_char do |c|
      if DOUBLE_ESCAPES.key?(c)
        put(DOUBLE_ESCAPES[c])
      else
        o = ARGV.size > 2 ? c : c.ord
        put(o <= 0xFF ? format("x%02X", o) : o <= 0xFFFF ? format("u%04X", o) : format("U%08X", o))
      end
    end
    mark!
    indent(30)
    @out
  end
end
puts Emitter.new.escape("a\né中\u{1F600}").inspect

class Pathish
  def to_s = "/custom"
end
def build(args = nil, **kw)
  h = args || kw
  path = h[:path].to_s
  path = path.start_with?("/") ? "/%2F#{path[1..-1]}" : "/#{path}"
  tc = h[:typecode]
  path = "#{path};type=#{tc}" unless tc.nil?
  path << "?q" if kw[:q]
  h[:path] = path
  h
end
p build(path: "/x"), build(path: "y", typecode: "i"), build({ path: nil }), build(path: ARGV.empty? ? Pathish.new : 3, q: 1)
