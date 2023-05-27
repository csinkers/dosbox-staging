﻿namespace DosboxDebugger;

public interface ITracer
{
    void Clear();
    void Debug(string message);
    void Info(string message);
    void Warn(string message);
    void Error(string message);
}