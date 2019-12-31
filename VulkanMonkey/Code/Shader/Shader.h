#pragma once

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

#include "../../include/shaderc/shaderc/shaderc.hpp"

#define ONLINE_COMPILE

namespace vm {

	struct Define
	{
		std::string name{};
		std::string value{};
	};

	enum struct ShaderType
	{
		Vertex,
		Fragment,
		Compute,
		Geometry,
		TessControl,
		TessEvaluation
	};

	struct FileIncluder : public shaderc::CompileOptions::IncluderInterface
	{
		shaderc_include_result* GetInclude(const char* requested_source, shaderc_include_type, const char* requesting_source, size_t) override;
		void ReleaseInclude(shaderc_include_result* include_result) override;
		inline const std::unordered_set<std::string>& file_path_trace() const { return included_files_; }
	
	private:
		struct FileInfo {
			const std::string full_path;
			std::vector<char> contents;
		};
		std::unordered_set<std::string> included_files_;
	};

	struct Shader
	{
		Shader(const std::string& filename, ShaderType kind, bool online_compile, const std::vector<Define>& defs = {});
		const uint32_t* get_spriv();
		size_t size();

	private:
		void init_source(const std::string& filename);
		void preprocess_shader(shaderc_shader_kind kind);
		void compile_file_to_assembly(shaderc_shader_kind kind);
		void compile_file(shaderc_shader_kind kind);
		void addDefine(Define& define);
		void addDefines(const std::vector<Define>& defines);

	private:
		shaderc::Compiler compiler;
		shaderc::CompileOptions options;
		std::string m_source_name{};
		std::string m_source{};
		std::string m_preprocessed{};
		std::string m_assembly{};
		std::vector<uint32_t> m_spirv{};
		std::vector<Define> defines{};
	};
}