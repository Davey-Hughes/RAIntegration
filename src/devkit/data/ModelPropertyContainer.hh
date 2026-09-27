#ifndef RA_DATA_MODEL_PROPERTY_CONTAINER_H
#define RA_DATA_MODEL_PROPERTY_CONTAINER_H
#pragma once

#include "data/ModelProperty.hh"

#include <mutex>

namespace ra {
namespace data {

class ModelPropertyContainer
{
public:
    GSL_SUPPRESS_F6 ModelPropertyContainer() = default;

#ifndef NDEBUG
    virtual ~ModelPropertyContainer() noexcept {
        m_bDestructed = true;
    }

    bool m_bDestructed = false;
#else
    virtual ~ModelPropertyContainer() noexcept = default;
#endif

    ModelPropertyContainer(const ModelPropertyContainer&) = delete;
    ModelPropertyContainer& operator=(const ModelPropertyContainer&) = delete;
    GSL_SUPPRESS_F6 ModelPropertyContainer(ModelPropertyContainer&&) = delete;
    GSL_SUPPRESS_F6 ModelPropertyContainer& operator=(ModelPropertyContainer&&) = delete;

    /// <summary>
    /// Gets the value associated to the requested boolean property.
    /// </summary>
    /// <param name="pProperty">The property to query.</param>
    /// <returns>The current value of the property for this object.</returns>
    bool GetValue(const BoolModelProperty& pProperty) const
    {
        int nValue = 0;
        return FindValue(pProperty.GetKey(), nValue) ? (nValue != 0) : pProperty.GetDefaultValue();
    }

    /// <summary>
    /// Sets the specified boolean property to the specified value.
    /// </summary>
    /// <param name="pProperty">The property to set.</param>
    /// <param name="bValue">The value to set.</param>
    void SetValue(const BoolModelProperty& pProperty, bool bValue);

    /// <summary>
    /// Called when a boolean value changes.
    /// </summary>
    /// <param name="args">Information about the change.</param>
    virtual void OnValueChanged(const BoolModelProperty::ChangeArgs& args) noexcept(false);

    /// <summary>
    /// Gets the value associated to the requested string property.
    /// </summary>
    /// <param name="pProperty">The property to query.</param>
    /// <returns>The current value of the property for this object.</returns>
    const std::wstring& GetValue(const StringModelProperty& pProperty) const
    {
        int nIndex = 0;
        if (!FindValue(pProperty.GetKey(), nIndex))
            return pProperty.GetDefaultValue();

        return GetString(nIndex);
    }

    /// <summary>
    /// Gets a copy of a string property's value, taken under the lock that
    /// <see cref="SetValue(const StringModelProperty&amp;, const std::wstring&amp;)" /> holds.
    /// </summary>
    /// <remarks>
    /// <see cref="GetValue(const StringModelProperty&amp;)" /> returns a reference into the slot that a
    /// <c>SetValue</c> on another thread may be overwriting at that moment. A reader on a thread of its own - a
    /// binding on the Qt views' thread - copies instead.
    /// </remarks>
    std::wstring CopyValue(const StringModelProperty& pProperty) const;

    /// <summary>
    /// Sets the specified string property to the specified value.
    /// </summary>
    /// <param name="pProperty">The property to set.</param>
    /// <param name="sValue">The value to set.</param>
    void SetValue(const StringModelProperty& pProperty, const std::wstring& sValue);

    /// <summary>
    /// Called when a string value changes.
    /// </summary>
    /// <param name="args">Information about the change.</param>
    virtual void OnValueChanged(const StringModelProperty::ChangeArgs& args) noexcept(false);

    /// <summary>
    /// Gets the value associated to the requested integer property.
    /// </summary>
    /// <param name="pProperty">The property to query.</param>
    /// <returns>The current value of the property for this object.</returns>
    int GetValue(const IntModelProperty& pProperty) const
    {
        int nValue = 0;
        return FindValue(pProperty.GetKey(), nValue) ? nValue : pProperty.GetDefaultValue();
    }

    /// <summary>
    /// Sets the specified integer property to the specified value.
    /// </summary>
    /// <param name="pProperty">The property to set.</param>
    /// <param name="nValue">The value to set.</param>
    void SetValue(const IntModelProperty& pProperty, int nValue);

    /// <summary>
    /// Called when a integer value changes.
    /// </summary>
    /// <param name="args">Information about the change.</param>
    virtual void OnValueChanged(const IntModelProperty::ChangeArgs& args) noexcept(false);

private:
    typedef struct ModelPropertyValue
    {
        ModelPropertyValue(int nKey, int nValue) noexcept
            : nKey(nKey), nValue(nValue)
        {
        }

        int nKey;
        int nValue;
    } ModelPropertyValue;
    std::vector<ModelPropertyValue> m_vValues;

    typedef struct ModelPropertyStrings
    {
        static constexpr size_t ChunkCount = 4;

        std::wstring sStrings[ChunkCount];
        std::unique_ptr<struct ModelPropertyStrings> pNext;
    } ModelPropertyStrings;
    std::unique_ptr<ModelPropertyStrings> m_pStrings;

#ifdef _DEBUG
    /// <summary>
    /// Complete list of values as strings for viewing in the debugger
    /// </summary>
    std::map<std::string, std::wstring> m_mDebugValues;
#endif

    static std::wstring s_sEmpty;

    static int CompareModelPropertyKey(const ModelPropertyValue& left, int nKey) noexcept;
    // Copies the value out under the lock: a pointer into m_vValues would outlive it, and a SetValue on another
    // thread that inserts or erases an entry moves - or frees - the one it points to.
    bool FindValue(int nKey, int& nValue) const;
    const std::wstring& GetString(int nIndex) const noexcept;
    int LoadIntoEmptyStringSlot(const std::wstring& sValue);

    mutable std::mutex m_mtxData;
};

} // namespace data
} // namespace ra

#endif RA_DATA_MODEL_PROPERTY_CONTAINER_H
