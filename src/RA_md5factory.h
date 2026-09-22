#ifndef RA_MD5FACTORY_H
#define RA_MD5FACTORY_H
#pragma once

extern std::string RAGenerateMD5(const std::string& sStringToMD5);
extern std::string RAGenerateMD5(const unsigned char* pIn, size_t nLen);
extern std::string RAGenerateMD5(const std::vector<unsigned char> DataIn);

extern std::string RAGenerateFileMD5(const std::wstring& sPath);

extern std::string RAFormatMD5(const unsigned char* digest);

#endif // !RA_MD5FACTORY_H
