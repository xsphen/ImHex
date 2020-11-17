#include "views/view_pattern.hpp"

#include "lang/preprocessor.hpp"
#include "lang/parser.hpp"
#include "lang/lexer.hpp"
#include "lang/validator.hpp"
#include "utils.hpp"

namespace hex {

    ViewPattern::ViewPattern(prv::Provider* &dataProvider, std::vector<hex::PatternData*> &patternData)
        : View(), m_dataProvider(dataProvider), m_patternData(patternData) {

        this->m_buffer = new char[0xFF'FFFF];
        std::memset(this->m_buffer, 0x00, 0xFF'FFFF);
    }
    ViewPattern::~ViewPattern() {
        if (this->m_buffer != nullptr)
            delete[] this->m_buffer;
    }

    void ViewPattern::createMenu() {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Load pattern...")) {
                View::doLater([]{ ImGui::OpenPopup("Open Hex Pattern"); });
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Pattern View", "", &this->m_windowOpen);
            ImGui::EndMenu();
        }
    }

    void ViewPattern::createView() {
        if (!this->m_windowOpen)
            return;

        if (ImGui::Begin("Pattern", &this->m_windowOpen, ImGuiWindowFlags_None)) {
            if (this->m_buffer != nullptr && this->m_dataProvider != nullptr && this->m_dataProvider->isReadable()) {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

                auto size = ImGui::GetWindowSize();
                size.y -= 50;
                ImGui::InputTextMultiline("Pattern", this->m_buffer, 0xFFFF, size,
                                          ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackEdit,
                                          [](ImGuiInputTextCallbackData *data) -> int {
                                              auto _this = static_cast<ViewPattern *>(data->UserData);

                                              _this->parsePattern(data->Buf);

                                              return 0;
                                          }, this
                );

                ImGui::PopStyleVar(2);
            }
        }
        ImGui::End();

        if (this->m_fileBrowser.showFileDialog("Open Hex Pattern", imgui_addons::ImGuiFileBrowser::DialogMode::OPEN, ImVec2(0, 0), ".hexpat")) {

            FILE *file = fopen(this->m_fileBrowser.selected_path.c_str(), "rb");

            if (file != nullptr) {
                fseek(file, 0, SEEK_END);
                size_t size = ftell(file);
                rewind(file);

                if (size >= 0xFF'FFFF) {
                    fclose(file);
                    return;
                }

                fread(this->m_buffer, size, 1, file);

                fclose(file);

                this->parsePattern(this->m_buffer);
            }
        }
    }


    void ViewPattern::addPatternData(PatternData *patternData) {
        this->m_patternData.push_back(patternData);
    }

    void ViewPattern::clearPatternData() {
        for (auto &data : this->m_patternData)
            delete data;

        this->m_patternData.clear();
        PatternData::resetPalette();
    }

    template<std::derived_from<lang::ASTNode> T>
    static std::vector<T*> findNodes(const lang::ASTNode::Type type, const std::vector<lang::ASTNode*> &nodes) {
        std::vector<T*> result;

        for (const auto & node : nodes)
            if (node->getType() == type)
                result.push_back(static_cast<T*>(node));

        return result;
    }

    void ViewPattern::parsePattern(char *buffer) {
        static hex::lang::Preprocessor preprocessor;
        static hex::lang::Lexer lexer;
        static hex::lang::Parser parser;
        static hex::lang::Validator validator;

        this->clearPatternData();
        this->postEvent(Events::PatternChanged);

        auto [preprocessingResult, preprocesedCode] = preprocessor.preprocess(buffer);
        if (preprocessingResult.failed())
            return;

        auto [lexResult, tokens] = lexer.lex(preprocesedCode);
        if (lexResult.failed()) {
            return;
        }

        auto [parseResult, ast] = parser.parse(tokens);
        if (parseResult.failed()) {
            for(auto &node : ast) delete node;
            return;
        }

        auto validatorResult = validator.validate(ast);
        if (!validatorResult) {
            for(auto &node : ast) delete node;
            return;
        }

        for (auto &varNode : findNodes<lang::ASTNodeVariableDecl>(lang::ASTNode::Type::VariableDecl, ast)) {
            if (!varNode->getOffset().has_value())
                continue;

            u64 offset = varNode->getOffset().value();
            if (varNode->getVariableType() != lang::Token::TypeToken::Type::CustomType) {
                size_t size = getTypeSize(varNode->getVariableType()) * varNode->getArraySize();

                if (isUnsigned(varNode->getVariableType()))
                    this->addPatternData(new PatternDataUnsigned(offset, size, varNode->getVariableName()));
                else if (isSigned(varNode->getVariableType())) {
                    if (getTypeSize(varNode->getVariableType()) == 1 && varNode->getArraySize() == 1)
                        this->addPatternData(new PatternDataCharacter(offset, size, varNode->getVariableName()));
                    else if (getTypeSize(varNode->getVariableType()) == 1 && varNode->getArraySize() > 1)
                        this->addPatternData(new PatternDataString(offset, size, varNode->getVariableName()));
                    else
                        this->addPatternData(new PatternDataSigned(offset, size, varNode->getVariableName()));
                }
                else if (isFloatingPoint(varNode->getVariableType()))
                    this->addPatternData(new PatternDataFloat(offset, size, varNode->getVariableName()));
            } else {
                for (auto &structNode : findNodes<lang::ASTNodeStruct>(lang::ASTNode::Type::Struct, ast)) {
                    if (varNode->getCustomVariableTypeName() == structNode->getName()) {
                        for (u32 i = 0; i < varNode->getArraySize(); i++) {
                            std::string name = varNode->getVariableName();
                            if (varNode->getArraySize() > 1)
                                name += "[" + std::to_string(varNode->getArraySize()) + "]";

                            if (size_t size = this->highlightStruct(ast, structNode, offset, name); size == -1)
                                this->clearPatternData();
                            else
                                offset += size;
                        }
                    }
                }

                for (auto &enumNode : findNodes<lang::ASTNodeEnum>(lang::ASTNode::Type::Enum, ast)) {
                    if (varNode->getCustomVariableTypeName() == enumNode->getName()) {
                        for (u32 i = 0; i < varNode->getArraySize(); i++) {
                            std::string name = varNode->getVariableName();
                            if (varNode->getArraySize() > 1)
                                name += "[" + std::to_string(varNode->getArraySize()) + "]";

                            if (size_t size = this->highlightEnum(ast, enumNode, offset, name); size == -1)
                                this->clearPatternData();
                            else
                                offset += size;
                        }
                    }
                }

                for (auto &usingNode : findNodes<lang::ASTNodeTypeDecl>(lang::ASTNode::Type::TypeDecl, ast)) {
                    if (varNode->getCustomVariableTypeName() == usingNode->getTypeName()) {
                        for (u32 i = 0; i < varNode->getArraySize(); i++) {
                            std::string name = varNode->getVariableName();
                            if (varNode->getArraySize() > 1)
                                name += "[" + std::to_string(varNode->getArraySize()) + "]";

                            if (size_t size = this->highlightUsingDecls(ast, usingNode, varNode, offset, name); size == -1)
                                this->clearPatternData();
                            else
                                offset += size;
                        }
                    }
                }
            }

        }

        for(auto &node : ast) delete node;
        this->postEvent(Events::PatternChanged);
    }

    s32 ViewPattern::highlightUsingDecls(std::vector<lang::ASTNode*> &ast, lang::ASTNodeTypeDecl* currTypeDeclNode, lang::ASTNodeVariableDecl* currVarDecl, u64 offset, std::string name) {
        u64 startOffset = offset;

        if (currTypeDeclNode->getAssignedType() != lang::Token::TypeToken::Type::CustomType) {
            size_t size = (static_cast<u32>(currTypeDeclNode->getAssignedType()) >> 4);

            if (isUnsigned(currTypeDeclNode->getAssignedType()))
                this->addPatternData(new PatternDataUnsigned(offset, size, name));
            else if (isSigned(currTypeDeclNode->getAssignedType()))
                this->addPatternData(new PatternDataSigned(offset, size, name));
            else if (isFloatingPoint(currTypeDeclNode->getAssignedType()))
                this->addPatternData(new PatternDataFloat(offset, size, name));

            offset += size;
        } else {
            bool foundType = false;
            for (auto &structNode : findNodes<lang::ASTNodeStruct>(lang::ASTNode::Type::Struct, ast)) {
                if (structNode->getName() == currTypeDeclNode->getAssignedCustomTypeName()) {
                    for (size_t i = 0; i < currVarDecl->getArraySize(); i++) {
                        size_t size = this->highlightStruct(ast, structNode, offset, name);

                        if (size == -1)
                            return -1;

                        offset += size;
                    }

                    foundType = true;
                    break;
                }
            }

            for (auto &enumNode : findNodes<lang::ASTNodeEnum>(lang::ASTNode::Type::Enum, ast)) {
                if (enumNode->getName() == currTypeDeclNode->getAssignedCustomTypeName()) {
                    for (size_t i = 0; i < currVarDecl->getArraySize(); i++) {
                        size_t size = this->highlightEnum(ast, enumNode, offset, name);

                        if (size == -1)
                            return -1;

                        offset += size;
                    }

                    foundType = true;
                    break;
                }
            }


            for (auto &typeDeclNode : findNodes<lang::ASTNodeTypeDecl>(lang::ASTNode::Type::TypeDecl, ast)) {
                if (typeDeclNode->getTypeName() == currTypeDeclNode->getAssignedCustomTypeName()) {
                    for (size_t i = 0; i < currVarDecl->getArraySize(); i++) {
                        size_t size = this->highlightUsingDecls(ast, typeDeclNode, currVarDecl, offset, name);

                        if (size == -1)
                            return -1;

                        offset += size;
                    }

                    foundType = true;
                    break;
                }
            }

            if (!foundType)
                return -1;
        }

        return offset - startOffset;
    }

    s32 ViewPattern::highlightStruct(std::vector<lang::ASTNode*> &ast, lang::ASTNodeStruct* currStructNode, u64 offset, std::string name) {
        u64 startOffset = offset;

        for (auto &node : currStructNode->getNodes()) {
            auto varNode = static_cast<lang::ASTNodeVariableDecl*>(node);

            if (varNode->getVariableType() != lang::Token::TypeToken::Type::CustomType) {
                size_t size = (static_cast<u32>(varNode->getVariableType()) >> 4);
                for (size_t i = 0; i < varNode->getArraySize(); i++) {
                    std::string memberName = name + "." + varNode->getVariableName();
                    if (varNode->getArraySize() > 1)
                        memberName += "[" + std::to_string(i) + "]";

                    if (isUnsigned(varNode->getVariableType()))
                        this->addPatternData(new PatternDataUnsigned(offset, size, memberName));
                    else if (isSigned(varNode->getVariableType())) {
                        if (getTypeSize(varNode->getVariableType()) == 1 && varNode->getArraySize() == 1)
                            this->addPatternData(new PatternDataCharacter(offset, size, memberName));
                        else if (getTypeSize(varNode->getVariableType()) == 1 && varNode->getArraySize() > 1) {
                            this->addPatternData(new PatternDataString(offset, size * varNode->getArraySize(), name + "." + varNode->getVariableName()));
                            offset += size * varNode->getArraySize();
                            break;
                        }
                        else
                            this->addPatternData(new PatternDataSigned(offset, size, memberName));
                    }
                    else if (isFloatingPoint(varNode->getVariableType()))
                        this->addPatternData(new PatternDataFloat(offset, size, memberName));

                    offset += size;
                }
            } else {
                bool foundType = false;
                for (auto &structNode : findNodes<lang::ASTNodeStruct>(lang::ASTNode::Type::Struct, ast)) {
                    if (structNode->getName() == varNode->getCustomVariableTypeName()) {
                        for (size_t i = 0; i < varNode->getArraySize(); i++) {
                            std::string memberName = name + "." + varNode->getVariableName();
                            if (varNode->getArraySize() > 1)
                                memberName += "[" + std::to_string(i) + "]";

                            size_t size = this->highlightStruct(ast, structNode, offset, memberName);

                            if (size == -1)
                                return -1;

                            offset += size;
                        }

                        foundType = true;
                        break;
                    }
                }

                for (auto &enumNode : findNodes<lang::ASTNodeEnum>(lang::ASTNode::Type::Enum, ast)) {
                    if (enumNode->getName() == varNode->getCustomVariableTypeName()) {
                        for (size_t i = 0; i < varNode->getArraySize(); i++) {
                            std::string memberName = name + "." + varNode->getVariableName();
                            if (varNode->getArraySize() > 1)
                                memberName += "[" + std::to_string(i) + "]";

                            size_t size = this->highlightEnum(ast, enumNode, offset, memberName);

                            if (size == -1)
                                return -1;

                            offset += size;
                        }

                        foundType = true;
                        break;
                    }
                }

                for (auto &typeDeclNode : findNodes<lang::ASTNodeTypeDecl>(lang::ASTNode::Type::TypeDecl, ast)) {
                    if (typeDeclNode->getTypeName() == varNode->getCustomVariableTypeName()) {
                        for (size_t i = 0; i < varNode->getArraySize(); i++) {
                            std::string memberName = name + "." + varNode->getVariableName();
                            if (varNode->getArraySize() > 1)
                                memberName += "[" + std::to_string(i) + "]";

                            size_t size = this->highlightUsingDecls(ast, typeDeclNode, varNode, offset, memberName);

                            if (size == -1)
                                return -1;

                            offset += size;
                        }

                        foundType = true;
                        break;
                    }
                }

                if (!foundType)
                    return -1;
            }

        }

        return offset - startOffset;
    }

    s32 ViewPattern::highlightEnum(std::vector<lang::ASTNode*> &ast, lang::ASTNodeEnum* currEnumNode, u64 offset, std::string name) {
        if (!isUnsigned(currEnumNode->getUnderlyingType()))
            return -1;

        s32 size = static_cast<u32>(currEnumNode->getUnderlyingType()) >> 4;

        if (size > 8)
            return -1;

        this->addPatternData(new PatternDataEnum(offset, size, name, currEnumNode->getName(), currEnumNode->getValues()));

        return size;
    }

}