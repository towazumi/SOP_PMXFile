#pragma once

#include <SOP/SOP_Node.h>
#include <UT/UT_StringHolder.h>

namespace PMX {


class SOP_PMXFile : public SOP_Node
{
public:
    static PRM_Template *buildTemplates();
    static OP_Node *myConstructor(OP_Network *net, const char *name, OP_Operator *op)
    {
        return new SOP_PMXFile(net, name, op);
    }

    static const UT_StringHolder theSOPTypeName;
    
    const SOP_NodeVerb *cookVerb() const override;

protected:
    SOP_PMXFile(OP_Network *net, const char *name, OP_Operator *op)
        : SOP_Node(net, name, op)
    {
        // All verb SOPs must manage data IDs, to track what's changed
        // from cook to cook.
        mySopFlags.setManagesDataIDs(true);
    }
    
    ~SOP_PMXFile() override {}

    /// Since this SOP implements a verb, cookMySop just delegates to the verb.
    OP_ERROR cookMySop(OP_Context &context) override
    {
        return cookMyselfAsVerb(context);
    }
};

} // End PMX namespace
