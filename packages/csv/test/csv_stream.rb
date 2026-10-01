# The parser reads bytes, so a field may hold any UTF-8 text and a col_sep or
# quote_char may be longer than one byte; CSV.foreach reads its file a record
# at a time, a quoted field carrying a record across line ends; CSV.parse_line
# parses only the first row. Every answer here is CRuby's.
require "csv"
require "tmpdir"

p CSV.parse("épée,naïve\n\"a, é\",\"say \"\"ï\"\"\"\n")
p CSV.parse("ü§ö§\n§x", col_sep: "§")
p CSV.parse("a::b:c::\"d::e\"", col_sep: "::")
p CSV.parse_line("a,é,\"\"\n\"unterminated")
p CSV.parse_line("\n\na")
p CSV.generate_line(["é\"ï", "plain", "a,b"])

path = File.join(Dir.tmpdir, "sp_csv_stream_#{Process.pid}.csv")
begin
  File.write(path, "h1,h2\r\n\"multi\nline\n\"\"field\"\"\",x\r\n\r\nplain,\"\"\r\nlast,é")
  rows = []
  CSV.foreach(path) { |r| rows << r }
  p rows
  rows = []
  CSV.foreach(path, skip_blanks: true) { |r| rows << r }
  p rows
  CSV.foreach(path, headers: true, skip_blanks: true) { |r| p r.to_h }

  File.write(path, "")
  rows = []
  CSV.foreach(path) { |r| rows << r }
  p rows

  # rows ahead of an unclosed quoted field are handed over before the error
  File.write(path, "a,b\nc,\"d\n")
  rows = []
  begin
    CSV.foreach(path) { |r| rows << r }
  rescue CSV::MalformedCSVError => e
    p e.class
  end
  p rows
ensure
  File.delete(path) if File.exist?(path)
end
