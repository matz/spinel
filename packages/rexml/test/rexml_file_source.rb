# REXML::Document.new given a File reads it to its end, as CRuby's does: a
# File.new, and the File a File.open block is handed.
require "rexml/document"
doc = REXML::Document.new(File.new(File.join(__dir__, "rexml_file_source.xml")))
puts doc.root.name
File.open(File.join(__dir__, "rexml_file_source.xml")) { |f| puts REXML::Document.new(f).root.elements["b"].text }
puts REXML::Document.new(File.read(File.join(__dir__, "rexml_file_source.xml"))).root.name
puts REXML::Document.new("<x/>").root.name
